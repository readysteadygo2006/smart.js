#include "esp8266.h"

#include <map>
#include <memory>
#include <string>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QtDebug>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <common/util/error_codes.h>
#include <common/util/statusor.h>

#include "fs.h"

// Code in this file (namely, rebootIntoBootloader function) assumes the same
// wiring as esptool.py:
//   RTS - CH_PD or RESET pin
//   DTR - GPIO0 pin

namespace ESP8266 {

namespace {

const ulong writeBlockSize = 0x400;
const ulong flashBlockSize = 4096;
const char fwFileGlob[] = "0x*.bin";
const ulong idBlockOffset = 0x10000;
const ulong idBlockSize = flashBlockSize;
const ulong spiffsBlockOffset = 0x6d000;
const ulong spiffsBlockSize = 0x10000;

// Copy-pasted from
// https://github.com/themadinventor/esptool/blob/e96336f6561109e67afe03c0695d1e5b0de15da6/esptool.py
// Copyright (C) 2014 Fredrik Ahlberg
// License: GPLv2
// Must be prefixed with 3 32-bit arguments: offset, blocklen, blockcount.
// Updated to reboot after reading.
const char ESPReadFlashStub[] =
    "\x80\x3c\x00\x40"  // data: send_packet
    "\x1c\x4b\x00\x40"  // data: SPIRead
    "\x80\x00\x00\x40"  // data: ResetVector
    "\x00\x80\xfe\x3f"  // data: buffer
    "\xc1\xfb\xff"      //       l32r    a12, $blockcount
    "\xd1\xf8\xff"      //       l32r    a13, $offset
    "\x2d\x0d"          // loop: mov.n   a2, a13
    "\x31\xfd\xff"      //       l32r    a3, $buffer
    "\x41\xf7\xff"      //       l32r    a4, $blocklen
    "\x4a\xdd"          //       add.n   a13, a13, a4
    "\x51\xf9\xff"      //       l32r    a5, $SPIRead
    "\xc0\x05\x00"      //       callx0  a5
    "\x21\xf9\xff"      //       l32r    a2, $buffer
    "\x31\xf3\xff"      //       l32r    a3, $blocklen
    "\x41\xf5\xff"      //       l32r    a4, $send_packet
    "\xc0\x04\x00"      //       callx0  a4
    "\x0b\xcc"          //       addi.n  a12, a12, -1
    "\x56\xec\xfd"      //       bnez    a12, loop
    "\x61\xf4\xff"      //       l32r    a6, $ResetVector
    "\xa0\x06\x00"      //       jx      a6
    "\x00\x00\x00"      //       // padding
    ;

// ESP8266 bootloader uses SLIP frame format for communication.
// https://tools.ietf.org/html/rfc1055
const unsigned char SLIPFrameDelimiter = 0xC0;
const unsigned char SLIPEscape = 0xDB;
const unsigned char SLIPEscapeFrameDelimiter = 0xDC;
const unsigned char SLIPEscapeEscape = 0xDD;

qint64 SLIP_write(QSerialPort* out, const QByteArray& bytes) {
  // XXX: errors are ignored.
  qDebug() << "Writing bytes: " << bytes.toHex();
  out->putChar(SLIPFrameDelimiter);
  for (int i = 0; i < bytes.length(); i++) {
    switch ((unsigned char) bytes[i]) {
      case SLIPFrameDelimiter:
        out->putChar(SLIPEscape);
        out->putChar(SLIPEscapeFrameDelimiter);
        break;
      case SLIPEscape:
        out->putChar(SLIPEscape);
        out->putChar(SLIPEscapeEscape);
        break;
      default:
        out->putChar(bytes[i]);
        break;
    }
  }
  out->putChar(SLIPFrameDelimiter);
  if (!out->waitForBytesWritten(200)) {
    qDebug() << "Error: " << out->errorString();
  }
  return bytes.length();
}

QByteArray SLIP_read(QSerialPort* in, int readTimeout = 200) {
  QByteArray ret;
  char c = 0;
  // Skip everything before the frame start.
  do {
    if (in->bytesAvailable() == 0 && !in->waitForReadyRead(readTimeout)) {
      qDebug() << "No data: " << in->errorString();
      return ret;
    }
    if (!in->getChar(&c)) {
      qDebug() << "Failed to read prefix: " << in->errorString();
      return ret;
    }
  } while ((unsigned char) c != SLIPFrameDelimiter);
  for (;;) {
    if (in->bytesAvailable() == 0 && !in->waitForReadyRead(readTimeout)) {
      qDebug() << "No data: " << in->errorString();
      return ret;
    }
    if (!in->getChar(&c)) {
      qDebug() << "Failed to read next char";
      return ret;
    }
    switch ((unsigned char) c) {
      case SLIPFrameDelimiter:
        // End of frame.
        qDebug() << "Read bytes: " << ret.toHex();
        return ret;
      case SLIPEscape:
        if (in->bytesAvailable() == 0 && !in->waitForReadyRead(readTimeout)) {
          qDebug() << "No data: " << in->errorString();
          return ret;
        }
        if (!in->getChar(&c)) {
          qDebug() << "Failed to read next char";
          return ret;
        }
        switch ((unsigned char) c) {
          case SLIPEscapeFrameDelimiter:
            ret.append(SLIPFrameDelimiter);
            break;
          case SLIPEscapeEscape:
            ret.append(SLIPEscape);
            break;
          default:
            qDebug() << "Invalid escape sequence: " << int(c);
            return ret;
        }
        break;
      default:
        ret.append(c);
        break;
    }
  }
}

quint8 checksum(const QByteArray& data) {
  quint8 r = 0xEF;
  for (int i = 0; i < data.length(); i++) {
    r ^= data[i];
  }
  return r;
}

void writeCommand(QSerialPort* out, quint8 cmd, const QByteArray& payload,
                  quint8 csum = 0) {
  QByteArray frame;
  QDataStream s(&frame, QIODevice::WriteOnly);
  s.setByteOrder(QDataStream::LittleEndian);
  s << quint8(0) << cmd << quint16(payload.length());
  s << quint32(csum);  // Yes, it is indeed padded with 3 zero bytes.
  frame.append(payload);
  SLIP_write(out, frame);
}

struct Response {
  quint8 command = 0xff;
  QByteArray value;
  QByteArray body;
  quint8 status = 0;
  quint8 lastError = 0;
  bool valid = false;

  bool ok() const {
    return valid && status == 0 && lastError == 0;
  }

  QString error() const {
    if (!valid) {
      return "invalid response";
    }
    if (status != 0 || lastError != 0) {
      return QString("status: %1 %2").arg(status).arg(lastError);
    }
    return "";
  }
};

Response readResponse(QSerialPort* in, int timeout = 200) {
  Response ret;
  ret.valid = false;
  QByteArray resp = SLIP_read(in, timeout);
  if (resp.length() < 10) {
    qDebug() << "Incomplete response: " << resp.toHex();
    return ret;
  }
  QDataStream s(resp);
  s.setByteOrder(QDataStream::LittleEndian);
  quint8 direction;
  s >> direction;
  if (direction != 1) {
    qDebug() << "Invalid direction (first byte) in response: " << resp.toHex();
    return ret;
  }

  quint16 size;
  s >> ret.command >> size;

  ret.value.resize(4);
  char* buf = ret.value.data();
  s.readRawData(buf, 4);

  ret.body.resize(size);
  buf = ret.body.data();
  s.readRawData(buf, size);
  if (ret.body.length() == 2) {
    ret.status = ret.body[0];
    ret.lastError = ret.body[1];
  }

  ret.valid = true;
  return ret;
}

QByteArray read_register(QSerialPort* serial, quint32 addr) {
  QByteArray payload;
  QDataStream s(&payload, QIODevice::WriteOnly);
  s.setByteOrder(QDataStream::LittleEndian);
  s << addr;
  writeCommand(serial, 0x0A, payload);
  auto resp = readResponse(serial);
  if (!resp.valid) {
    qDebug() << "Invalid response to command " << 0x0A;
    return QByteArray();
  }
  if (resp.command != 0x0A) {
    qDebug() << "Response to unexpected command: " << resp.command;
    return QByteArray();
  }
  if (resp.status != 0) {
    qDebug() << "Bad response status: " << resp.status;
    return QByteArray();
  }
  return resp.value;
}

QByteArray read_MAC(QSerialPort* serial) {
  QByteArray ret;
  auto mac1 = read_register(serial, 0x3ff00050);
  auto mac2 = read_register(serial, 0x3ff00054);
  if (mac1.length() != 4 || mac2.length() != 4) {
    return ret;
  }
  QDataStream s(&ret, QIODevice::WriteOnly);
  s.setByteOrder(QDataStream::LittleEndian);
  switch (mac2[2]) {
    case 0:
      s << quint8(0x18) << quint8(0xFE) << quint8(0x34);
      break;
    case 1:
      s << quint8(0xAC) << quint8(0xD0) << quint8(0x74);
      break;
    default:
      qDebug() << "Unknown OUI";
      return ret;
  }
  s << quint8(mac2[1]) << quint8(mac2[0]) << quint8(mac1[3]);
  return ret;
}

bool sync(QSerialPort* serial) {
  QByteArray payload("\x07\x07\x12\x20");
  payload.append(QByteArray("\x55").repeated(32));
  writeCommand(serial, 0x08, payload);
  for (int i = 0; i < 8; i++) {
    auto resp = readResponse(serial);
    if (!resp.valid) {
      return false;
    }
  }
  return true;
}

bool trySync(QSerialPort* serial, int attempts) {
  for (; attempts > 0; attempts--) {
    if (sync(serial)) {
      return true;
    }
  }
  return false;
}

bool rebootIntoBootloader(QSerialPort* serial) {
  serial->setDataTerminalReady(false);
  serial->setRequestToSend(true);
  QThread::msleep(50);
  serial->setDataTerminalReady(true);
  serial->setRequestToSend(false);
  QThread::msleep(50);
  serial->setDataTerminalReady(false);
  return trySync(serial, 3);
}

void rebootIntoFirmware(QSerialPort* serial) {
  serial->setDataTerminalReady(false);  // pull up GPIO0
  serial->setRequestToSend(true);       // pull down RESET
  QThread::msleep(50);
  serial->setRequestToSend(false);  // pull up RESET
}

}  // namespace

bool probe(const QSerialPortInfo& port) {
  QSerialPort s(port);
  if (!s.setBaudRate(QSerialPort::Baud9600)) {
    qDebug() << "Failed to set baud rate on " << port.systemLocation();
    return false;
  }
  if (!s.setParity(QSerialPort::NoParity)) {
    qDebug() << "Failed to disable parity on " << port.systemLocation();
    return false;
  }
  if (!s.setFlowControl(QSerialPort::NoFlowControl)) {
    qDebug() << "Failed to disable flow control on " << port.systemLocation();
    return false;
  }
  if (!s.open(QIODevice::ReadWrite)) {
    qDebug() << "Failed to open " << port.systemLocation();
    return false;
  }

  if (!rebootIntoBootloader(&s)) {
    return false;
  }

  auto mac = read_MAC(&s).toHex();
  if (mac.length() < 6) {
    return false;
  }
  qDebug() << "MAC address: " << mac;

  return true;
}

class FlasherImpl : public Flasher {
  Q_OBJECT
 public:
  explicit FlasherImpl(bool preserve_flash_params, bool erase_bug_workaround,
                       qint32 override_flash_params,
                       bool merge_flash_filesystem,
                       bool generate_id_if_none_found, QString id_hostname)
      : preserve_flash_params_(preserve_flash_params),
        erase_bug_workaround_(erase_bug_workaround),
        override_flash_params_(override_flash_params),
        merge_flash_filesystem_(merge_flash_filesystem),
        generate_id_if_none_found_(generate_id_if_none_found),
        id_hostname_(id_hostname) {
  }

  QString load(const QString& path) override {
    QMutexLocker lock(&lock_);
    images_.clear();
    QDir dir(path, fwFileGlob, QDir::Name,
             QDir::Files | QDir::NoDotAndDotDot | QDir::Readable);
    if (!dir.exists()) {
      return tr("directory does not exist");
    }

    const auto files = dir.entryInfoList();
    if (files.length() == 0) {
      return tr("no files to flash");
    }
    for (const auto& file : files) {
      qDebug() << "Loading" << file.fileName();
      bool ok = false;
      ulong addr = file.baseName().toULong(&ok, 16);
      if (!ok) {
        return tr("%1 is not a valid address").arg(file.baseName());
      }
      QFile f(file.absoluteFilePath());
      if (!f.open(QIODevice::ReadOnly)) {
        return tr("failed to open %1").arg(file.absoluteFilePath());
      }
      auto bytes = f.readAll();
      if (bytes.length() != file.size()) {
        images_.clear();
        return tr("%1 has size %2, but readAll returned %3 bytes")
            .arg(file.fileName(), file.size(), bytes.length());
      }
      images_[addr] = bytes;
    }
    return "";
  }

  QString setPort(QSerialPort* port) override {
    QMutexLocker lock(&lock_);
    port_ = port;
    return "";
  }

  int totalBlocks() const override {
    QMutexLocker lock(&lock_);
    int r = 0;
    for (const auto& bytes : images_.values()) {
      r += bytes.length() / writeBlockSize;
      if (bytes.length() % writeBlockSize != 0) {
        r++;
      }
    }
    return r;
  }

  void run() override {
    QMutexLocker lock(&lock_);

    if (!rebootIntoBootloader(port_)) {
      emit done(tr("Failed to talk to bootloader. See <a "
                   "href=\"https://github.com/cesanta/smart.js/blob/master/"
                   "platforms/esp8266/flashing.md\">wiring instructions</a>."),
                false);
      return;
    }

    int flashParams = -1;
    if (override_flash_params_ >= 0) {
      flashParams = override_flash_params_;
    } else if (preserve_flash_params_) {
      // Here we're trying to read 2 bytes from already flashed firmware and
      // copy them to the image we're about to write. These 2 bytes (bytes 2 and
      // 3, counting from zero) are the paramters of the flash chip needed by
      // ESP8266 SDK code to properly boot the device.
      // TODO(imax): before reading from flash try to check if have the correct
      // params for the flash chip by its ID.
      QByteArray params = readFlashParamsLocked();
      if (params.length() == 2) {
        qWarning() << "Current flash params bytes:" << params.toHex();
        flashParams = params[0] << 8 | params[1];
      } else {
        qWarning() << "Failed to read flash params";
        emit done(tr("failed to read flash params from the existing firmware"),
                  false);
        return;
      }
    }

    if (images_.contains(0) && images_[0].length() >= 4 &&
        images_[0][0] == (char) 0xE9) {
      if (flashParams >= 0) {
        images_[0][2] = (flashParams >> 8) & 0xff;
        images_[0][3] = flashParams & 0xff;
        qWarning() << "Adjusting flash params in the image 0x0000 to"
                   << images_[0].mid(2, 2).toHex();
      }
      flashParams = images_[0][2] << 8 | images_[0][3];
    }

// TODO(imax): enable back once it works reliably without aborting flashing.
#if 0
    if (false && merge_flash_filesystem_) {
      auto res = mergeFlashLocked();
      if (res.ok()) {
        images_[spiffsBlockOffset] = res.ValueOrDie();
        qWarning() << "Merged flash content";
      } else {
        qWarning() << "Failed to merge flash content:"
                   << res.status().ToString().c_str();
        emit done(tr("failed to merge flash filesystem"), false);
        return;
      }
    }
#endif

    if (generate_id_if_none_found_) {
      auto res = findIdLocked();
      if (res.ok()) {
        if (!res.ValueOrDie()) {
          qWarning() << "Generating new ID";
          images_[idBlockOffset] = generateIdBlock(id_hostname_);
        } else {
          qWarning() << "Existing ID found";
        }
      } else {
        qWarning() << "Failed to read existing ID block:"
                   << res.status().ToString().c_str();
        emit done(tr("failed to check for ID presence"), false);
        return;
      }
    }

    written_count_ = 0;
    for (ulong addr : images_.keys()) {
      bool success = false;
      int written = written_count_;

      for (int attempts = 2; attempts >= 0; attempts--) {
        if (writeFlashLocked(addr, images_[addr])) {
          success = true;
          break;
        }
        qWarning() << "Failed to write image at" << hex << showbase << addr
                   << "," << dec << attempts << "attempts left";
        written_count_ = written;
        emit progress(written_count_);
        if (!rebootIntoBootloader(port_)) {
          break;
        }
      }
      if (!success) {
        emit done(tr("failed to flash image at 0x%1").arg(addr, 0, 16), false);
        return;
      }
    }

    switch ((flashParams >> 8) & 0xff) {
      case 2:  // DIO
        // This is a workaround for ROM switching flash in DIO mode to
        // read-only. See https://github.com/nodemcu/nodemcu-firmware/pull/523
        rebootIntoFirmware(port_);
        break;
      default:
        if (!leaveFlashingModeLocked()) {
          emit done(
              tr("failed to leave flashing mode. Most likely flashing was "
                 "successful, but you need to reboot your device manually."),
              false);
          return;
        }
        break;
    }

    emit done(tr("All done!"), true);
  }

 private:
  static ulong fixupEraseLength(ulong start, ulong len) {
    // This function allows to offset for SPIEraseArea bug in ESP8266 ROM,
    // making it erase at most one extra 4KB sector.
    //
    // Flash chips used with ESP8266 have 4KB sectors grouped into 64KB blocks.
    // SPI commands allow to erase each sector separately and the whole block at
    // once, so SPIEraseArea attempts to be smart and first erase sectors up to
    // the end of the block, then continue with erasing in blocks and again
    // erase a few sectors in the beginning of the last block.
    // But it does not subtract the number of sectors erased in the first block
    // from the total number of sectors to erase, so that number gets erased
    // twice. Also, due to the way it is written, even if you tell it to erase
    // range starting and ending at the block boundary it will erase first and
    // last block sector by sector.
    // The number of sectors erased is a function of 2 arguments:
    // f(x,t) = 2*x, if x <= t
    //          x+t, if x > t
    // Where `x` - number of sectors to erase, `t` - number of sectors to erase
    // in the first block (that is, 16 if we start at the block boundary).
    // To offset that we don't pass `x` directly, but some function of `x` and
    // `t`:
    // g(x,t) = x/2 + x%1, if x <= 2*t
    //          x-t      , if x > 2*t
    // Results of pieces of `g` fall in the same ranges as inputs of
    // corresponding pieces of `f`, so it's a bit easier to express their
    // composition:
    // f(g(x,t),t) = 2*(x/2 + x%1) = x + x%1, if g(x,t) <= t
    //               (x-t) + t = x          , if g(x,t) > t
    // So, the worst case is when you need to erase odd number of sectors less
    // than `2*t`, then one extra sector will be erased, in all other cases no
    // extra sectors are erased.
    const ulong sectorSize = 4096;
    const ulong sectorsPerBlock = 16;
    start /= sectorSize;
    ulong tail = sectorsPerBlock - start % sectorsPerBlock;
    ulong sectors = len / sectorSize;
    if (len % sectorSize != 0) {
      sectors++;
    }
    if (sectors <= 2 * tail) {
      return (sectors / 2 + (sectors % 2)) * sectorSize;
    } else {
      return len - tail * sectorSize;
    }
  }

  bool writeFlashLocked(ulong addr, const QByteArray& bytes) {
    const ulong blocks = bytes.length() / writeBlockSize +
                         (bytes.length() % writeBlockSize == 0 ? 0 : 1);
    qDebug() << "Writing" << blocks << "blocks at" << hex << showbase << addr;
    emit statusMessage(tr("Erasing flash at 0x%1...").arg(addr, 0, 16));
    if (!writeFlashStartLocked(addr, blocks)) {
      qDebug() << "Failed to start flashing";
      return false;
    }
    for (ulong start = 0; start < ulong(bytes.length());
         start += writeBlockSize) {
      QByteArray data = bytes.mid(start, writeBlockSize);
      if (ulong(data.length()) < writeBlockSize) {
        data.append(
            QByteArray("\xff").repeated(writeBlockSize - data.length()));
      }
      qDebug() << "Writing block" << start / writeBlockSize;
      emit statusMessage(tr("Writing block %1@0x%2...")
                             .arg(start / writeBlockSize)
                             .arg(addr, 0, 16));
      if (!writeFlashBlockLocked(start / writeBlockSize, data)) {
        qDebug() << "Failed to write block" << start / writeBlockSize;
        return false;
      }
      written_count_++;
      emit progress(written_count_);
    }
    return true;
  }

  bool writeFlashStartLocked(ulong addr, ulong blocks) {
    QByteArray payload;
    QDataStream s(&payload, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    if (erase_bug_workaround_) {
      s << quint32(fixupEraseLength(addr, blocks * writeBlockSize));
    } else {
      s << quint32(blocks * writeBlockSize);
    }
    s << quint32(blocks) << quint32(writeBlockSize) << quint32(addr);
    qDebug() << "Attempting to start flashing...";
    writeCommand(port_, 0x02, payload);
    const auto resp = readResponse(port_, 30000);
    if (!resp.ok()) {
      qDebug() << "Failed to enter flashing mode." << resp.error();
      return false;
    }
    return true;
  }

  bool writeFlashBlockLocked(int seq, const QByteArray& bytes) {
    QByteArray payload;
    QDataStream s(&payload, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s << quint32(bytes.length()) << quint32(seq) << quint32(0) << quint32(0);
    payload.append(bytes);
    writeCommand(port_, 0x03, payload, checksum(bytes));
    const auto resp = readResponse(port_, 10000);
    if (!resp.ok()) {
      qDebug() << "Failed to write flash block." << resp.error();
      return false;
    }
    return true;
  }

  bool leaveFlashingModeLocked() {
    QByteArray payload("\x01\x00\x00\x00", 4);
    writeCommand(port_, 0x04, payload);
    const auto resp = readResponse(port_, 10000);
    if (!resp.ok()) {
      qDebug() << "Failed to leave flashing mode." << resp.error();
      if (erase_bug_workaround_) {
        // Error here is expected, Espressif's esptool.py ignores it as well.
        return true;
      }
      return false;
    }
    return true;
  }

  util::StatusOr<QByteArray> readFlashLocked(ulong offset, ulong len) {
    // Init flash.
    if (!writeFlashStartLocked(0, 0)) {
      return util::Status(util::error::ABORTED, "failed to initialize flash");
    }

    QByteArray stub;
    QDataStream s(&stub, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s << quint32(offset) << quint32(len) << quint32(1);
    stub.append(ESPReadFlashStub, sizeof(ESPReadFlashStub) - 1);

    QByteArray payload;
    QDataStream s1(&payload, QIODevice::WriteOnly);
    s1.setByteOrder(QDataStream::LittleEndian);
    s1 << quint32(stub.length()) << quint32(1) << quint32(stub.length())
       << quint32(0x40100000);

    writeCommand(port_, 0x05, payload);
    auto resp = readResponse(port_);
    if (!resp.ok()) {
      qDebug() << "Failed to start writing to RAM." << resp.error();
      return util::Status(util::error::ABORTED,
                          "failed to start writing to RAM");
    }

    payload.clear();
    QDataStream s2(&payload, QIODevice::WriteOnly);
    s2.setByteOrder(QDataStream::LittleEndian);
    s2 << quint32(stub.length()) << quint32(0) << quint32(0) << quint32(0);
    payload.append(stub);
    qDebug() << "Stub length:" << showbase << hex << stub.length();
    writeCommand(port_, 0x07, payload, checksum(stub));
    resp = readResponse(port_);
    if (!resp.ok()) {
      qDebug() << "Failed to write to RAM." << resp.error();
      return util::Status(util::error::ABORTED, "failed to write to RAM");
    }

    payload.clear();
    QDataStream s3(&payload, QIODevice::WriteOnly);
    s3.setByteOrder(QDataStream::LittleEndian);
    s3 << quint32(0) << quint32(0x4010001c);
    writeCommand(port_, 0x06, payload);
    resp = readResponse(port_);
    if (!resp.ok()) {
      qDebug() << "Failed to complete writing to RAM." << resp.error();
      return util::Status(util::error::ABORTED, "failed to initialize flash");
    }

    QByteArray r = SLIP_read(port_);
    if (r.length() < int(len)) {
      qDebug() << "Failed to read flash.";
      return util::Status(util::error::ABORTED, "failed to read flash");
    }

    if (!trySync(port_, 5)) {
      qWarning() << "Device did not reboot after reading flash";
      return util::Status(util::error::ABORTED,
                          "failed to jump to bootloader after reading flash");
    }

    return r;
  }

  // readFlashParamsLocked puts a snippet of code in the RAM and executes it.
  // You need to reboot the device again after that to talk to the bootloader
  // again.
  QByteArray readFlashParamsLocked() {
    auto res = readFlashLocked(0, 4);
    if (!res.ok()) {
      qWarning() << "Reading flash params failed: "
                 << res.status().ToString().c_str();
      return QByteArray();
    }

    QByteArray r = res.ValueOrDie();
    if (r[0] != (char) 0xE9) {
      qDebug() << "Read image doesn't seem to have the proper header.";
      return QByteArray();
    }
    return r.mid(2, 2);
  }

  // mergeFlashLocked reads the spiffs filesystem from the device
  // and mounts it in memory. Then it overwrites the files that are
  // present in the software update but it leaves the existing ones.
  // The idea is that the filesystem is mostly managed by the user
  // or by the software update utility, while the core system uploaded by
  // the flasher should only upload a few core files.
  util::StatusOr<QByteArray> mergeFlashLocked() {
    auto res = readFlashLocked(spiffsBlockOffset, spiffsBlockSize);
    if (!res.ok()) {
      return res.status();
    }

    SPIFFS bundled(images_[spiffsBlockOffset]);
    SPIFFS dev(res.ValueOrDie());

    auto err = dev.merge(bundled);
    if (!err.ok()) {
      return err;
    }
    return dev.data();
  }

  util::StatusOr<bool> findIdLocked() {
    // Block with ID has the following structure:
    // 1) 20-byte SHA-1 hash of the payload
    // 2) payload (JSON object)
    // 3) 1-byte terminator ('\0')
    // 4) padding with 0xFF bytes to the block size
    auto res = readFlashLocked(idBlockOffset, idBlockSize);
    if (!res.ok()) {
      return res.status();
    }

    QByteArray r = res.ValueOrDie();
    const int SHA1Length = 20;
    QByteArray hash = r.left(SHA1Length);
    int terminator = r.indexOf('\0', SHA1Length);
    if (terminator < 0) {
      return false;
    }
    return hash ==
           QCryptographicHash::hash(r.mid(SHA1Length, terminator - SHA1Length),
                                    QCryptographicHash::Sha1);
  }

  static QByteArray generateIdBlock(QString id_hostname) {
    qsrand(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);
    QByteArray random;
    QDataStream s(&random, QIODevice::WriteOnly);
    for (int i = 0; i < 6; i++) {
      // Minimal value for RAND_MAX is 32767, so we are guaranteed to get at
      // least 15 bits of randomness. In that case highest bit of each word will
      // be 0, but whatever, we're not doing crypto here (although we should).
      s << qint16(qrand() & 0xFFFF);
      // TODO(imax): use a proper cryptographic PRNG at least for PSK. It must
      // be hard to guess PSK knowing the ID, which is not the case with
      // qrand(): there are at most 2^32 unique sequences.
    }
    QByteArray data =
        QString("{\"id\":\"//%1/d/%2\",\"key\":\"%3\"}")
            .arg(id_hostname)
            .arg(QString::fromUtf8(
                random.mid(0, 5).toBase64(QByteArray::Base64UrlEncoding |
                                          QByteArray::OmitTrailingEquals)))
            .arg(QString::fromUtf8(
                random.mid(5).toBase64(QByteArray::Base64UrlEncoding |
                                       QByteArray::OmitTrailingEquals)))
            .toUtf8();
    QByteArray r = QCryptographicHash::hash(data, QCryptographicHash::Sha1)
                       .append(data)
                       .append("\0", 1);
    r.append(QByteArray("\xFF").repeated(idBlockSize - r.length()));
    return r;
  }

  mutable QMutex lock_;
  QMap<ulong, QByteArray> images_;
  QSerialPort* port_;
  int written_count_ = 0;
  bool preserve_flash_params_ = true;
  bool erase_bug_workaround_ = true;
  qint32 override_flash_params_ = -1;
  bool merge_flash_filesystem_ = true;
  bool generate_id_if_none_found_ = true;
  QString id_hostname_;
};

std::unique_ptr<Flasher> flasher(bool preserveFlashParams,
                                 bool eraseBugWorkaround,
                                 qint32 overrideFlashParams,
                                 bool mergeFlashFilesystem,
                                 bool generateIdIfNoneFound,
                                 QString idHostname) {
  return std::move(std::unique_ptr<Flasher>(new FlasherImpl(
      preserveFlashParams, eraseBugWorkaround, overrideFlashParams,
      mergeFlashFilesystem, generateIdIfNoneFound, idHostname)));
}

namespace {

using std::map;
using std::string;

const map<string, int> flashMode = {
    {"qio", 0}, {"qout", 1}, {"dio", 2}, {"dout", 3},
};

const map<string, int> flashSize = {
    {"4m", 0},
    {"2m", 1},
    {"8m", 2},
    {"16m", 3},
    {"32m", 4},
    {"16m-c1", 5},
    {"32m-c1", 6},
    {"32m-c2", 7},
};

const map<string, int> flashFreq = {
    {"40m", 0}, {"26m", 1}, {"20m", 2}, {"80m", 0xf},
};
}

util::StatusOr<int> flashParamsFromString(const QString& s) {
  QStringList parts = s.split(',');
  switch (parts.size()) {
    case 1: {  // number
      bool ok = false;
      int r = s.toInt(&ok, 0);
      if (!ok) {
        return util::Status(util::error::INVALID_ARGUMENT, "invalid number");
      }
      return r & 0xffff;
    }
    case 3:  // string
      if (flashMode.find(parts[0].toStdString()) == flashMode.end()) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "invalid flash mode");
      }
      if (flashSize.find(parts[1].toStdString()) == flashSize.end()) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "invalid flash size");
      }
      if (flashFreq.find(parts[2].toStdString()) == flashFreq.end()) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "invalid flash frequency");
      }
      return (flashMode.find(parts[0].toStdString())->second << 8) |
             (flashSize.find(parts[1].toStdString())->second << 4) |
             (flashFreq.find(parts[2].toStdString())->second);
    default:
      return util::Status(
          util::error::INVALID_ARGUMENT,
          "must be either a number or a comma-separated list of three items");
  }
}

}  // namespace ESP8266

#include "esp8266.moc"
