# Flashing ESP8266

## Wiring

At the bare minimum, you need to have serial TX & RX connected to the RX and TX
pins on ESP8266 board. To enter flashing mode GPIO0 must be pulled down on boot,
so you can simply connect it to GND before connecting power.

Other option is to connect GPIO0 to DTR (Data Terminal Ready) line of serial
adapter and RESET pin to RTS (Request To Send) line. This is the way it is wired
in NodeMCU boards and what `esptool.py` also assumes.

Summary:

* GND - ground
* TX - serial RX
* RX - serial TX
* VCC - power
* CH_PD - power
* GPIO0 - ground or serial DTR
* RESET - leave unconnected or serial RTS

## Flash’n’Chips

If you have DTR and RTS connected properly, pressing "Load firmware" button
should reboot the device automatically. Otherwise, just connect GPIO0 to ground
and reset the device manually before flashing.

## esptool

As `esptool.py` does not generate the device ID, you need to do this manually if
you want to connect the device to our cloud. To do this invoke `mkid.py` script:

```
./tools/mkid.py --id ${ID} --psk ${PSK} > 0x10000.bin
```

Replace `${ID}` with a few random characters and `${PSK}` with a password unique
to this device. After that just flash `0x10000.bin` at offset 0x10000 along with
other pieces:

```
path/to/esptool.py -b 115200 -p /dev/ttyUSB0 write_flash 0x00000 0x00000.bin 0x10000 0x10000.bin 0x1d000 0xd1000.bin 0x28000 0x28000.bin
```

For NodeMCU v2 board don't forget to also add
`--flash_mode dio --flash_size 32m` flags after `write_flash` or the device
won't boot.
