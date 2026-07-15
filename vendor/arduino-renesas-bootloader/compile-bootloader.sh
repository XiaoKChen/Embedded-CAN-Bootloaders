#!/bin/bash

# Argument Validation
TARGET_BOARD=$1
case "$TARGET_BOARD" in
    "R4-wifi"|"R4-minima"|"PortentaC33"|"Opta-Analog"|"Opta-Digital"|"All-boards")
        # Valid argument, proceed
        ;;
    *)
        echo "Error: Invalid or missing target board."
        echo "Usage: $0 [R4-minima|R4-wifi|PortentaC33|Opta-Analog|Opta-Digital|All-boards]"
        exit 1
        ;;
esac

# Determine Execution Context (Inside repo vs Generic workspace)
if [ -f "0001-fix-arduino-bootloaders.patch" ] && [ -f "compile.sh" ]; then
    IN_REPO=true
    WORKSPACE_DIR="$PWD"
    REPO_DIR="$PWD"
    # If tinyusb is cloned here, the patch is one folder up from inside tinyusb
    PATCH_PATH="../0001-fix-arduino-bootloaders.patch" 
else
    IN_REPO=false
    WORKSPACE_DIR="$PWD"
    REPO_DIR="$PWD/arduino-renesas-bootloader"
    # If tinyusb is cloned next to the bootloader repo, the patch is in the sibling folder
    PATCH_PATH="../arduino-renesas-bootloader/0001-fix-arduino-bootloaders.patch"
fi

# Workspace Confirmation
if [ ! -d "tinyusb" ]; then
    echo "==================================================="
    echo "WORKSPACE SETUP"
    if [ "$IN_REPO" = true ]; then
        echo "Detected execution from INSIDE the arduino-renesas-bootloader repository."
        echo "tinyusb will be cloned directly into this folder:"
        echo "-> $WORKSPACE_DIR"
        echo "Proceeding automatically..."
    else
        echo "Detected execution from a generic workspace folder."
        echo "Repositories will be cloned/checked in this folder:"
        echo "-> $WORKSPACE_DIR"

        read -p "Do you want to move the script to a different folder before proceeding? (y/N): " move_choice
        case "$move_choice" in
            y|Y ) 
                echo "Script execution terminated. Please move the script to your desired folder and run it again."
                exit 0 
                ;;
            * ) 
                echo "Proceeding with the current folder..." 
                ;;
        esac
    fi
    echo "==================================================="
fi

# Check the ARM GCC compiler
COMPILER_VERSION=$(arm-none-eabi-gcc --version 2>/dev/null | head -n 1)

if [ -z "$COMPILER_VERSION" ]; then
    echo "Error: arm-none-eabi-gcc not found in PATH."
    exit 1
fi

echo "Current Compiler: $COMPILER_VERSION"

# If the compiler string does NOT contain "(Build arm-"
if [[ "$COMPILER_VERSION" != *"(Build arm-"* ]]; then
    echo "---------------------------------------------------"
    echo "WARNING: Unable to determine if this is an official ARM compiler."
    echo "You should use an official ARM compiler that can be downloaded from here:"
    echo "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
    echo "---------------------------------------------------"
    
    read -p "Do you want to continue the process with the current compiler? (y/N): " choice
    case "$choice" in
        y|Y ) echo "Continuing compilation..." ;;
        * ) echo "Compilation stopped by user."; exit 1 ;;
    esac
fi

# Setup Repositories
echo "==================================================="
echo "Checking repositories..."

if [ "$IN_REPO" = false ]; then
    if [ ! -d "arduino-renesas-bootloader" ]; then
        echo "Cloning arduino-renesas-bootloader..."
        git clone https://github.com/arduino/arduino-renesas-bootloader
    else
        echo "arduino-renesas-bootloader already exists. Skipping clone."
    fi
fi

if [ ! -d "tinyusb" ]; then
    echo "Cloning tinyusb..."
    git clone https://github.com/hathach/tinyusb
    cd tinyusb || exit 1
    git checkout 0.17.0
    
    echo "Patching tinyusb..."
    patch -p1 < "$PATCH_PATH"
    
    echo "Getting tinyusb dependencies..."
    if ! python3 tools/get_deps.py ra; then
        echo "Error: Failed to fetch tinyusb dependencies. Ensure python3 is installed and working."
        exit 1
    fi
    cd ..
else
    echo "tinyusb already exists. Skipping clone and setup."
fi

# Automatically set TINYUSB_ROOT if not set in the environment
if [ -z "$TINYUSB_ROOT" ]; then
    export TINYUSB_ROOT="$WORKSPACE_DIR/tinyusb"
    echo "Automatically set TINYUSB_ROOT to $TINYUSB_ROOT"
fi

# Move into the bootloader directory for compilation
cd "$REPO_DIR" || exit 1
PROJECT_NAME=$(basename "$PWD")

# Clean and prepare directories
if [ -d _build ]; then
    rm -R _build
fi

if [ -d distrib ]; then
    rm -R distrib
fi

mkdir distrib

# Function to handle compiling, size checking, cleanup, and hex recreation
build_board() {
    local makefile=$1
    local build_dir=$2
    local out_hex_name=$3
    local size_limit=$4
    local skip_recreation=$5

    echo "==================================================="
    echo "Building $out_hex_name..."
    
    # Check if make succeeds
    if ! make -f "$makefile" -j8; then
        echo "Error: Compilation failed for $out_hex_name. Stopping script."
        exit 1
    fi

    local base_path="$build_dir/$PROJECT_NAME"

   

    # Check the dimension of the .bin file against the specific board limit
    local bin_size=$(wc -c < "${base_path}.bin")

    if [ "$bin_size" -le "$size_limit" ]; then
        echo "Size check passed: .bin file is $bin_size bytes (Limit: $size_limit bytes)."
        
        if [ "$skip_recreation" = "true" ]; then
            echo "Skipping hex file recreation for $out_hex_name..."
            # Directly copy the original hex file
            cp "${base_path}.hex" "distrib/$out_hex_name"
        else
            # Delete the original hex file first since we are recreating it
            rm -f "${base_path}.hex"

            # Recreate the hex file without Extended address region
            local new_hex="${base_path}_new.hex"
            arm-none-eabi-objcopy -I binary -O ihex --change-addresses=0x00000000 "${base_path}.bin" "$new_hex"

            cp "$new_hex" "distrib/$out_hex_name"
        fi

        # Now delete the bin file
        rm -f "${base_path}.bin"

        echo "SUCCESS: Process successfully completed."
        echo "You can find the new bootloader '$out_hex_name' in the distrib sub-folder under arduino-renesas-bootloader."
    else
        echo "FAILED: Bootloader compilation failed for $out_hex_name."
        echo "Reason: The .bin file is too big ($bin_size bytes). Maximum allowed is $size_limit bytes."
        echo "Please switch to an ARM based build before attempting again."
        
        # Clean up the large bin file
        rm -f "${base_path}.bin"
        rm -f "${base_path}.hex"
        rm -f "${base_path}.elf"
    fi

    # Clean up the build directory for this specific board run
    echo "clean up build folder"
    rm -rf "$build_dir"
}

# Build the selected board based on the argument
case "$TARGET_BOARD" in
    "R4-wifi")
        build_board "Makefile.wifi" "_build/uno_r4" "dfu_wifi.hex" 16384 "false"
        ;;
    "R4-minima")
        build_board "Makefile.minima" "_build/uno_r4" "dfu_minima.hex" 16384 "false"
        ;;
    "PortentaC33")
        build_board "Makefile.c33" "_build/portenta_c33" "dfu_c33.hex" 65535 "true"
        ;;
    "Opta-Analog")
        build_board "Makefile.opta-analog" "_build/uno_r4" "opta-analog.hex" 32760 "false"
        ;;
    "Opta-Digital")
        build_board "Makefile.opta-digital" "_build/uno_r4" "opta-digital.hex" 32760 "false"
        ;;
    "All-boards")
        build_board "Makefile.wifi" "_build/uno_r4" "dfu_wifi.hex" 16384 "false"
        build_board "Makefile.minima" "_build/uno_r4" "dfu_minima.hex" 16384 "true"
        build_board "Makefile.c33" "_build/portenta_c33" "dfu_c33.hex" 65535 "true"
        build_board "Makefile.opta-analog" "_build/uno_r4" "opta-analog.hex" 32760 "true"
        build_board "Makefile.opta-digital" "_build/uno_r4" "opta-digital.hex" 32760 "true"
        ;;
esac

echo "==================================================="
echo "Script execution finished."
