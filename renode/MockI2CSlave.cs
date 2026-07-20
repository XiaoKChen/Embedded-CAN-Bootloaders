//
// MockI2CSlave.cs — a minimal I2C register-file slave for validating the
// ra4m1-hal RIIC master driver (crates/ra4m1-hal/src/i2c.rs) against Renode
// v1.16.1's stock `I2C.RenesasRA_IIC` model.  MIT-licensed; a test-time Renode
// peripheral, never linked into firmware.
//
// WHY THIS EXISTS
// ---------------
// The stock `RenesasRA_IIC` is a bus MASTER model that routes a transaction to a
// registered `II2CPeripheral` SLAVE (see RenesasRA_IIC.WriteData/ReadData:
// selectedPeripheral.Write(...) / .Read(count) / .FinishTransmission()).  The
// bootloader repo ships no slave, so a master-driven transaction has nothing to
// talk to.  This models the classic 8-bit-register-pointer slave (the shape of
// almost every I2C EEPROM/sensor): a WRITE sets the register pointer then
// streams data at auto-incrementing addresses; a READ returns bytes from the
// pointer, auto-incrementing.
//
// WHAT THIS VALIDATES (and what it does NOT)
// ------------------------------------------
// Driven through the stock RIIC engine by the real firmware driver, it validates
// the driver's register-logic handshake: START -> address(+R/W ACK) -> data ->
// (repeated-START) -> STOP.  It does NOT validate any silicon fact — SCL timing,
// rise/fall, clock stretching, arbitration, and analog behaviour are all absent.
// The RIIC master model performs transfers instantly (bar a fixed 100us address
// delay it schedules), so no bit-timing is exercised here.
//
// POINTER RETENTION ACROSS A REPEATED-START (the load-bearing detail)
// -------------------------------------------------------------------
// A `write_read` (write the register pointer, repeated-START, read) reaches this
// slave as: Write([ptr]) then FinishTransmission() (the RIIC flushes the write
// buffer and finishes the sub-transaction on the repeated-START — see
// RenesasRA_IIC.HandleRestartCondition -> HandleStopCondition), then Read(...).
// FinishTransmission therefore must NOT reset the address pointer — only the
// "next written byte is the pointer" latch — or the following read would start
// from 0 instead of the just-written register.  The pointer persists; the latch
// resets.
//
using Antmicro.Renode.Core;
using Antmicro.Renode.Peripherals;
using Antmicro.Renode.Peripherals.I2C;

namespace Antmicro.Renode.Peripherals.I2C
{
    public class MockI2CSlave : II2CPeripheral
    {
        public MockI2CSlave(IMachine machine)
        {
            registers = new byte[RegisterCount];
            Reset();
        }

        public void Reset()
        {
            pointer = 0;
            expectPointer = true;
            TransactionCount = 0;
            for(var i = 0; i < registers.Length; i++)
            {
                registers[i] = 0;
            }
        }

        // Master write: the first byte of a fresh (sub-)transaction is the
        // register pointer; subsequent bytes are stored at the auto-incrementing
        // address.
        public void Write(byte[] data)
        {
            foreach(var b in data)
            {
                if(expectPointer)
                {
                    pointer = b;
                    expectPointer = false;
                }
                else
                {
                    registers[pointer] = b;
                    pointer = (byte)(pointer + 1);
                }
            }
        }

        // Master read: return `count` bytes from the pointer, auto-incrementing.
        public byte[] Read(int count = 1)
        {
            var result = new byte[count];
            for(var i = 0; i < count; i++)
            {
                result[i] = registers[pointer];
                pointer = (byte)(pointer + 1);
            }
            return result;
        }

        // End of a (sub-)transaction: re-arm the pointer latch but KEEP the
        // pointer so a repeated-START read continues from the written register.
        public void FinishTransmission()
        {
            expectPointer = true;
            TransactionCount++;
        }

        // Monitor-facing helpers so a Robot suite can preload/inspect the file
        // and assert the master addressed this slave.
        public void SetRegister(int address, byte value)
        {
            registers[(byte)address] = value;
        }

        public byte GetRegister(int address)
        {
            return registers[(byte)address];
        }

        // Number of completed (sub-)transactions in which this slave was
        // addressed — a monitor-visible proof that the master's START/address/
        // STOP reached it.
        public uint TransactionCount { get; private set; }

        private byte pointer;
        private bool expectPointer;

        private readonly byte[] registers;

        private const int RegisterCount = 256;
    }
}
