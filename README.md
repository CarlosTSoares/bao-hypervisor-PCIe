# Bao - a lightweight static partitioning hypervisor

Bao source code in development for PCIe support. This branch is specific to the work in progress regarding the ITS sharing among two guest OSes.
This work expands the one-vm-msi feature.
ITS sharing allows more than one VM to use MSI interrupts. Only the following setups were tested:

- Qemu-virt: Linux + Baremetal


### Qemu-virt: Linux + Baremetal


In this setup, both Linux and a custom Baremetal app use the MSI mechanism to trigger interrupts. In Linux, PCIe network card uses LPIs 8192, 8193, 8194 to deliver MSIs to the virtual CPU. The Baremetal app uses the 8195 ID to trigger an MSI each time the timer interrupt handler is reached.

The ITS sharing only supports GICv3 spec yet.