//
// PMW3610 optical-sensor simulation for Renode.
//
// Implements just enough of the PixArt PMW3610's SPI register protocol for
// this module's driver (src/pmw3610.c) to complete its async init sequence
// and report motion end to end under emulation:
//
//   - single-register reads:  TX [addr], then data bytes clocked out
//   - single-register writes: TX [addr|0x80, value]
//   - the power-up/self-test handshake (0x3A=0x5A reset, OBSERVATION low
//     nibble reads back 0x0F, PRODUCT_ID reads 0x3E)
//   - MOTION_BURST (0x12) streaming of the 7-byte motion report
//   - FRAME_GRAB-armed (0x36=0x80) MOTION_BURST streaming of a synthetic
//     484-byte frame whose read pointer resets on every chip-select edge,
//     faithfully reproducing the real sensor's "NCS must stay low across
//     the whole frame readout" constraint the driver works around
//   - paged writes via 0x7F (page select), so CPI lands in page1/0x05
//
// Renode's NRF52840_SPI model never calls FinishTransmission(), so
// transaction framing is driven by the chip-select GPIO instead: the .repl
// file routes the CS pin (gpio1.15, xiao_d 10) to this peripheral's GPIO
// input 0. The MOTION output pin (active low, like the real part) is
// exposed as `MotionPin` and routed back to gpio1.14 (xiao_d 9).
//
// Test hooks (callable from the Renode monitor):
//   trackball QueueMotion 16 -8      queue one motion report (12-bit deltas)
//   trackball PendingMotionCount     motion reports not yet read
//   trackball ReadRegister 0x00      current effective register value
//   trackball WrittenValue 0 0x1B    last value the firmware wrote (page, addr)
//   trackball WriteCountTo 0 0x1B    write count to (page, addr)
//   trackball SetShutter 100         raise/lower the reported shutter value
//

using System.Collections.Generic;
using Antmicro.Renode.Core;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals;

namespace Antmicro.Renode.Peripherals.SPI
{
    public class PMW3610 : ISPIPeripheral, IGPIOReceiver
    {
        public PMW3610()
        {
            MotionPin = new GPIO();
            Reset();
        }

        public void Reset()
        {
            lock(locker)
            {
                ResetChip();
                motionQueue.Clear();
                UpdateMotionPin();
            }
        }

        // GPIO input 0 = chip select, physical level (true = high = deasserted).
        public void OnGPIO(int number, bool value)
        {
            if(number != 0)
            {
                this.Log(LogLevel.Warning, "Unexpected GPIO input {0}", number);
                return;
            }
            lock(locker)
            {
                var asserted = !value;
                if(asserted == csAsserted)
                {
                    return;
                }
                csAsserted = asserted;
                bytePosition = 0;
                readAddress = NoAddress;
                writeAddress = NoAddress;
                burstIndex = 0;
                if(!asserted)
                {
                    // Frame-grab read pointer resets whenever NCS goes high;
                    // motion pin reflects whatever is still queued.
                    frameIndex = 0;
                    UpdateMotionPin();
                }
            }
        }

        public byte Transmit(byte data)
        {
            lock(locker)
            {
                if(!csAsserted)
                {
                    this.Log(LogLevel.Warning, "SPI byte 0x{0:X2} with chip select deasserted", data);
                }

                byte result = 0x00;
                if(bytePosition == 0)
                {
                    if((data & SpiWriteBit) != 0)
                    {
                        writeAddress = data & 0x7F;
                        readAddress = NoAddress;
                    }
                    else
                    {
                        readAddress = data;
                        writeAddress = NoAddress;
                        burstIndex = 0;
                        if(readAddress == RegMotionBurst && !frameGrabArmed)
                        {
                            LatchMotionReport();
                        }
                    }
                }
                else if(writeAddress != NoAddress)
                {
                    if(bytePosition == 1)
                    {
                        HandleWrite(writeAddress, data);
                    }
                    else
                    {
                        this.Log(LogLevel.Warning, "Extra write byte 0x{0:X2} to 0x{1:X2}", data, writeAddress);
                    }
                }
                else if(readAddress != NoAddress)
                {
                    result = HandleReadByte();
                }
                bytePosition++;
                return result;
            }
        }

        public void FinishTransmission()
        {
            // Never called by Renode's NRF52840_SPI model -- CS framing is
            // handled in OnGPIO() instead.
        }

        public GPIO MotionPin { get; }

        // ------------------------------------------------------------------
        // Test hooks (Renode monitor / harness API)
        // ------------------------------------------------------------------

        public void QueueMotion(int deltaX, int deltaY)
        {
            lock(locker)
            {
                motionQueue.Enqueue(new MotionReport { DeltaX = deltaX, DeltaY = deltaY });
                UpdateMotionPin();
            }
        }

        public int PendingMotionCount()
        {
            lock(locker)
            {
                return motionQueue.Count;
            }
        }

        public byte ReadRegister(int address)
        {
            lock(locker)
            {
                return EffectiveRead((byte)address);
            }
        }

        public int WrittenValue(int page, int address)
        {
            lock(locker)
            {
                int value;
                return lastWritten.TryGetValue(RegKey(page, address), out value) ? value : -1;
            }
        }

        public int WriteCountTo(int page, int address)
        {
            lock(locker)
            {
                int count;
                return writeCounts.TryGetValue(RegKey(page, address), out count) ? count : 0;
            }
        }

        public void SetShutter(int value)
        {
            lock(locker)
            {
                shutter = value & 0x1FF;
            }
        }

        public int PowerUpResetCount()
        {
            lock(locker)
            {
                return powerUpResets;
            }
        }

        // ------------------------------------------------------------------
        // Internals
        // ------------------------------------------------------------------

        private void ResetChip()
        {
            currentPage = 0;
            csAsserted = false;
            bytePosition = 0;
            readAddress = NoAddress;
            writeAddress = NoAddress;
            burstIndex = 0;
            frameIndex = 0;
            frameGrabArmed = false;
            shutter = 20; // below the driver's smart-algorithm threshold (45)
            latched = new MotionReport();
            registers.Clear();
            // Motion queue, write history and reset counter survive a chip
            // reset on purpose: they are test-observability state, not chip
            // state.
        }

        private void HandleWrite(int address, byte value)
        {
            var key = RegKey(currentPage, address);
            lastWritten[key] = value;
            int count;
            writeCounts.TryGetValue(key, out count);
            writeCounts[key] = count + 1;

            // Page select works from either page (the CPI write sequence
            // switches back to page0 while still on page1).
            if(address == RegSpiPage0)
            {
                currentPage = value == 0xFF ? 1 : 0;
                return;
            }

            if(currentPage == 0)
            {
                switch(address)
                {
                case RegPowerUpReset:
                    if(value == PowerUpResetCommand)
                    {
                        powerUpResets++;
                        ResetChip();
                    }
                    return;
                case RegFrameGrab:
                    frameGrabArmed = (value & 0x80) != 0;
                    frameIndex = 0;
                    return;
                }
            }
            registers[key] = value;
        }

        private byte HandleReadByte()
        {
            if(readAddress == RegMotionBurst)
            {
                if(frameGrabArmed)
                {
                    var pixel = FramePixel(frameIndex);
                    frameIndex++;
                    return pixel;
                }
                var b = MotionBurstByte(burstIndex);
                burstIndex++;
                return b;
            }
            return EffectiveRead((byte)readAddress);
        }

        private byte EffectiveRead(byte address)
        {
            switch(address)
            {
            case RegProductId:
                return ProductId;
            case RegRevisionId:
                return 0x01;
            case RegObservation:
                // Self-test bits: the driver clears this register and expects
                // the low nibble to read back 0x0F once the (instantaneous,
                // here) self-test completes.
                return 0x0F;
            case RegMotion:
                LatchMotionReport();
                return MotionBurstByte(0);
            case RegDeltaXL:
                return MotionBurstByte(1);
            case RegDeltaYL:
                return MotionBurstByte(2);
            case RegDeltaXYH:
                return MotionBurstByte(3);
            }
            int value;
            return registers.TryGetValue(RegKey(currentPage, address), out value) ? (byte)value : (byte)0x00;
        }

        // Motion burst layout (matches src/pmw3610.h *_POS): [MOTION, X_L,
        // Y_L, XY_H, SQUAL, SHUTTER_H, SHUTTER_L]; deltas are 12-bit two's
        // complement, X high nibble in XY_H[7:4], Y high nibble in XY_H[3:0].
        private byte MotionBurstByte(int index)
        {
            var dx = latched.DeltaX & 0xFFF;
            var dy = latched.DeltaY & 0xFFF;
            switch(index)
            {
            case 0:
                return (byte)(latched.Valid ? 0x80 : 0x00);
            case 1:
                return (byte)(dx & 0xFF);
            case 2:
                return (byte)(dy & 0xFF);
            case 3:
                return (byte)(((dx >> 8) << 4) | (dy >> 8));
            case 4:
                return 0x40; // SQUAL: plausible tracking quality
            case 5:
                return (byte)((shutter >> 8) & 0x01);
            case 6:
                return (byte)(shutter & 0xFF);
            default:
                return 0x00;
            }
        }

        private void LatchMotionReport()
        {
            latched = motionQueue.Count > 0 ? motionQueue.Dequeue() : new MotionReport();
        }

        private void UpdateMotionPin()
        {
            // Active low: drive high (true) when idle, low when motion is
            // pending. The driver uses a level interrupt, so holding the pin
            // low until the queue drains re-triggers reads as expected.
            MotionPin.Set(motionQueue.Count == 0);
        }

        private static byte FramePixel(int index)
        {
            // Deterministic synthetic image so a future frame-capture test
            // can assert exact pixel values at any offset.
            return (byte)((index * 7 + 13) & 0xFF);
        }

        private static int RegKey(int page, int address)
        {
            return (page << 8) | (address & 0xFF);
        }

        private struct MotionReport
        {
            public int DeltaX;
            public int DeltaY;
            public bool Valid { get { return DeltaX != 0 || DeltaY != 0; } }
        }

        private readonly object locker = new object();
        private readonly Queue<MotionReport> motionQueue = new Queue<MotionReport>();
        private readonly Dictionary<int, int> registers = new Dictionary<int, int>();
        private readonly Dictionary<int, int> lastWritten = new Dictionary<int, int>();
        private readonly Dictionary<int, int> writeCounts = new Dictionary<int, int>();

        private bool csAsserted;
        private int bytePosition;
        private int readAddress;
        private int writeAddress;
        private int burstIndex;
        private int frameIndex;
        private bool frameGrabArmed;
        private int currentPage;
        private int shutter;
        private int powerUpResets;
        private MotionReport latched;

        private const int NoAddress = -1;
        private const byte SpiWriteBit = 0x80;
        private const byte ProductId = 0x3E;
        private const byte PowerUpResetCommand = 0x5A;

        private const int RegProductId = 0x00;
        private const int RegRevisionId = 0x01;
        private const int RegMotion = 0x02;
        private const int RegDeltaXL = 0x03;
        private const int RegDeltaYL = 0x04;
        private const int RegDeltaXYH = 0x05;
        private const int RegMotionBurst = 0x12;
        private const int RegObservation = 0x2D;
        private const int RegFrameGrab = 0x36;
        private const int RegPowerUpReset = 0x3A;
        private const int RegSpiPage0 = 0x7F;
    }
}
