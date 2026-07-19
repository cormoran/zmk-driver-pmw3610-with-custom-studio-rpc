//
// NRF52840_GPIO with a working LATCH register, for level/edge-sensitive GPIO
// interrupts under Renode.
//
// Why this exists: ZMK's PMW3610 driver requests a level-triggered interrupt
// on the sensor's motion pin (gpio_pin_interrupt_configure_dt with
// GPIO_INT_LEVEL_ACTIVE). On nRF52840, Zephyr/nrfx implements that via the
// GPIO SENSE mechanism + the GPIOTE PORT event, and its PORT-event ISR
// (nrfx_gpiote.c port_event_handle -> nrfy_gpio_latches_read_and_clear)
// reads the GPIO LATCH register to find which pin(s) triggered. Renode's
// stock NRF52840_GPIO models the PORT/DETECT path (the IRQ does fire) but
// leaves LATCH as an unimplemented tag that always reads 0 -- so the ISR
// finds no latched pin and dismisses the interrupt without ever calling the
// driver's callback. The sensor's motion IRQ is therefore silently dropped.
//
// This subclass adds the missing register: a LATCH bit is set when a pin's
// input level matches its configured SENSE, reads return the latched bits,
// and a write clears the bits set in the written value (write-1-to-clear),
// matching nRF hardware. Everything else is inherited unchanged, so it drops
// into the platform exactly where NRF52840_GPIO would and remains compatible
// with NRF52840_GPIOTasksEvents (which takes its ports typed as
// NRF52840_GPIO).
//
using Antmicro.Renode.Core;
using Antmicro.Renode.Core.Structure.Registers;

namespace Antmicro.Renode.Peripherals.GPIOPort
{
    public class NRF52840_GPIO_WithLatch : NRF52840_GPIO
    {
        public NRF52840_GPIO_WithLatch(IMachine machine) : base(machine)
        {
            // A pin latches when its (new) input level matches its SENSE
            // configuration -- exactly the condition nRF hardware uses to set
            // LATCH and raise the PORT DETECT signal.
            PinChanged += (pin, value) =>
            {
                var sensing = (pin.SenseMode == SenseMode.High && value)
                           || (pin.SenseMode == SenseMode.Low && !value);
                if(sensing)
                {
                    latch |= 1u << pin.Id;
                }
            };

            // The base class defines LATCH (0x20) as an inert tag; override its
            // read/write behaviour via collection hooks rather than
            // redefining the register (the base's Registers enum is private).
            RegistersCollection.AddBeforeReadHook(LatchOffset, _ => latch);
            RegistersCollection.AddAfterWriteHook(LatchOffset, (_, value) => latch &= ~value);
        }

        public override void Reset()
        {
            base.Reset();
            latch = 0;
        }

        private uint latch;

        private const long LatchOffset = 0x20;
    }
}
