#include "qemu/osdep.h"
#include "hw/vmstate-if.h"
#include "hw/intc/intc.h"
#include "accel/accel-cpu.h"
#include "system/event-loop-base.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/acpi/acpi_aml_interface.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_slave.h"

/* Force-link the Xbox machine registration unit from the static lib. */
extern void xbox_init_common(void);
__attribute__((used)) static void *xemu_android_force_xbox_machine_ref =
    (void *)&xbox_init_common;
/* Force-link the PC speaker device registration unit from the static lib. */
extern void xemu_android_force_pcspk_link(void);
__attribute__((used)) static void *xemu_android_force_pcspk_ref =
    (void *)&xemu_android_force_pcspk_link;
/* Force-link the NVNet device registration unit from the static lib. */
extern void xemu_android_force_nvnet_link(void);
__attribute__((used)) static void *xemu_android_force_nvnet_ref =
    (void *)&xemu_android_force_nvnet_link;
/* Force-link the xemu display backend registration unit from the static lib. */
extern void xemu_android_force_xemu_display_link(void);
__attribute__((used)) static void *xemu_android_force_xemu_display_ref =
    (void *)&xemu_android_force_xemu_display_link;
/* Force-link the null chardev backend registration unit from the static lib. */
extern void xemu_android_force_char_null_link(void);
__attribute__((used)) static void *xemu_android_force_char_null_ref =
    (void *)&xemu_android_force_char_null_link;
/* Force-link the RAM memory-backend registration unit from the static lib. */
extern void xemu_android_force_mem_backend_ram_link(void);
__attribute__((used)) static void *xemu_android_force_mem_backend_ram_ref =
    (void *)&xemu_android_force_mem_backend_ram_link;
/* Force-link the TCG accel ops registration unit from the static lib. */
extern void xemu_android_force_tcg_accel_ops_link(void);
__attribute__((used)) static void *xemu_android_force_tcg_accel_ops_ref =
    (void *)&xemu_android_force_tcg_accel_ops_link;
/* Force-link the ISA PIT device registration unit from the static lib. */
extern void xemu_android_force_i8254_link(void);
__attribute__((used)) static void *xemu_android_force_i8254_ref =
    (void *)&xemu_android_force_i8254_link;
/* Force-link the PIIX IDE device registration unit from the static lib. */
extern void xemu_android_force_piix_ide_link(void);
__attribute__((used)) static void *xemu_android_force_piix_ide_ref =
    (void *)&xemu_android_force_piix_ide_link;
/* Force-link the IDE device registration unit from the static lib. */
extern void xemu_android_force_ide_dev_link(void);
__attribute__((used)) static void *xemu_android_force_ide_dev_ref =
    (void *)&xemu_android_force_ide_dev_link;
/* Force-link the PCI OHCI controller registration unit from the static lib. */
extern void xemu_android_force_ohci_pci_link(void);
__attribute__((used)) static void *xemu_android_force_ohci_pci_ref =
    (void *)&xemu_android_force_ohci_pci_link;
/* Force-link the MCPX ACI device registration unit from the static lib. */
extern void xemu_android_force_mcpx_aci_link(void);
__attribute__((used)) static void *xemu_android_force_mcpx_aci_ref =
    (void *)&xemu_android_force_mcpx_aci_link;
/* Force-link the noaudio backend registration unit from the static lib. */
extern void xemu_android_force_noaudio_link(void);
__attribute__((used)) static void *xemu_android_force_noaudio_ref =
    (void *)&xemu_android_force_noaudio_link;
/* Force-link the PCI test device registration unit from the static lib. */
extern void xemu_android_force_pci_testdev_link(void);
__attribute__((used)) static void *xemu_android_force_pci_testdev_ref =
    (void *)&xemu_android_force_pci_testdev_link;
/* Force-link the SMBus storage device registration unit from the static lib. */
extern void xemu_android_force_smbus_storage_link(void);
__attribute__((used)) static void *xemu_android_force_smbus_storage_ref =
    (void *)&xemu_android_force_smbus_storage_link;
/* Force-link the USB hub device registration unit from the static lib. */
extern void xemu_android_force_usb_hub_link(void);
__attribute__((used)) static void *xemu_android_force_usb_hub_ref =
    (void *)&xemu_android_force_usb_hub_link;
/* Force-link the Xbox USB gamepad registration unit from the static lib. */
extern void xemu_android_force_xid_gamepad_link(void);
__attribute__((used)) static void *xemu_android_force_xid_gamepad_ref =
    (void *)&xemu_android_force_xid_gamepad_link;

#define XEMU_ANDROID_ACCEL_CPU_TYPE "accel-i386-cpu"

void xemu_android_register_vmstate_if(void)
{
    if (!object_class_by_name(TYPE_VMSTATE_IF)) {
        static const TypeInfo vmstate_if_info = {
            .name = TYPE_VMSTATE_IF,
            .parent = TYPE_INTERFACE,
            .class_size = sizeof(VMStateIfClass),
        };

        type_register_static(&vmstate_if_info);
    }

    if (!object_class_by_name(TYPE_INTERRUPT_STATS_PROVIDER)) {
        static const TypeInfo intctrl_info = {
            .name = TYPE_INTERRUPT_STATS_PROVIDER,
            .parent = TYPE_INTERFACE,
            .class_size = sizeof(InterruptStatsProviderClass),
        };

        type_register_static(&intctrl_info);
    }

    if (!object_class_by_name(XEMU_ANDROID_ACCEL_CPU_TYPE)) {
        static const TypeInfo accel_cpu_info = {
            .name = XEMU_ANDROID_ACCEL_CPU_TYPE,
            .parent = TYPE_OBJECT,
            .abstract = true,
            .class_size = sizeof(AccelCPUClass),
        };

        type_register_static(&accel_cpu_info);
    }

    if (!object_class_by_name(TYPE_EVENT_LOOP_BASE)) {
        static const TypeInfo event_loop_base_info = {
            .name = TYPE_EVENT_LOOP_BASE,
            .parent = TYPE_OBJECT,
            .abstract = true,
            .class_size = sizeof(EventLoopBaseClass),
            .instance_size = sizeof(EventLoopBase),
        };

        type_register_static(&event_loop_base_info);
    }

    if (!object_class_by_name(TYPE_FW_CFG_DATA_GENERATOR_INTERFACE)) {
        static const TypeInfo fw_cfg_data_generator_info = {
            .name = TYPE_FW_CFG_DATA_GENERATOR_INTERFACE,
            .parent = TYPE_INTERFACE,
            .class_size = sizeof(FWCfgDataGeneratorClass),
        };

        type_register_static(&fw_cfg_data_generator_info);
    }

    if (!object_class_by_name(TYPE_ACPI_DEV_AML_IF)) {
        static const TypeInfo acpi_dev_aml_if_info = {
            .name = TYPE_ACPI_DEV_AML_IF,
            .parent = TYPE_INTERFACE,
            .class_size = sizeof(AcpiDevAmlIfClass),
        };

        type_register_static(&acpi_dev_aml_if_info);
    }

    if (!object_class_by_name(TYPE_I2C_SLAVE)) {
        static const TypeInfo i2c_slave_info = {
            .name = TYPE_I2C_SLAVE,
            .parent = TYPE_DEVICE,
            .abstract = true,
            .class_size = sizeof(I2CSlaveClass),
            .instance_size = sizeof(I2CSlave),
        };

        type_register_static(&i2c_slave_info);
    }

    if (!object_class_by_name(TYPE_I2C_BUS)) {
        static const TypeInfo i2c_bus_info = {
            .name = TYPE_I2C_BUS,
            .parent = TYPE_BUS,
            .instance_size = sizeof(I2CBus),
        };

        type_register_static(&i2c_bus_info);
    }

    if (!object_class_by_name(TYPE_SMBUS_DEVICE)) {
        static const TypeInfo smbus_device_info = {
            .name = TYPE_SMBUS_DEVICE,
            .parent = TYPE_I2C_SLAVE,
            .abstract = true,
            .class_size = sizeof(SMBusDeviceClass),
            .instance_size = sizeof(SMBusDevice),
        };

        type_register_static(&smbus_device_info);
    }
}
