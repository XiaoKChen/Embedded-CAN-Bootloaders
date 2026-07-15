**Arduino bootloader for Renesas boards**

# Instructions to build Bootloaders

Ensure that `compile-bootloader.sh` is executable.
If it is not run:
`chmod +x compile-bootloader.sh`

Then execute the script and follow the instructions automatically displayed by
the script.

The script requires an argument that specifies what bootloader has to be build
and can assume the following values:

- R4-wifi
- R4-minima
- PortentaC33
- Opta-Analog
- Opta-Digital
- All-boards

## OLD instructions **_ DEPRECATED _** to build bootloader

This section is kept just for documentation and describe the manual compilation
process (this does not enforce any checks performed by the automatic script).

**The instructions here should not be used to build bootloaders (see previous
section)**

```
git clone https://github.com/arduino/arduino-renesas-bootloader
git clone https://github.com/hathach/tinyusb.git
cd tinyusb
git checkout 0.17.0
python ./tools/get_deps.py ra
export TINYUSB_ROOT=$PWD
patch -p1 < ../arduino-renesas-bootloader/0001-fix-arduino-bootloaders.patch
cd ..
cd arduino-renesas-bootloader
./compile.sh
```
