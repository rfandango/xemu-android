/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"

MCPXAPUState *g_state; // Used via debug handlers

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        // fprintf(stderr, "mcpx irq raise ien=%08x ists=%08x\n",
        //         d->regs[NV_PAPU_IEN], d->regs[NV_PAPU_ISTS]);
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        // fprintf(stderr, "mcpx irq lower ien=%08x ists=%08x\n",
        //         d->regs[NV_PAPU_IEN], d->regs[NV_PAPU_ISTS]);
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

static uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100; //???
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    trace_mcpx_apu_reg_read(addr, size, r);
    return r;
}

static void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    MCPXAPUState *d = opaque;

    trace_mcpx_apu_reg_write(addr, size, val);

    switch (addr) {
    case NV_PAPU_ISTS:
        /* the bits of the interrupts to clear are written */
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~val);
        update_irq(d);
        qemu_cond_signal(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], val);
        qemu_cond_signal(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write'
         * This value is expected to be written to FEMEMADDR on completion of
         * something to do with notifies. Just do it now :/ */
        stl_le_phys(&address_space_memory, d->regs[NV_PAPU_FEMEMADDR], val);
        // fprintf(stderr, "MAGIC WRITE\n");
        qatomic_set(&d->regs[addr], val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], val);
        }
        break;
    }
}

static const MemoryRegionOps mcpx_apu_mmio_ops = {
    .read = mcpx_apu_read,
    .write = mcpx_apu_write,
};

static int monitor_num_used_bytes(MCPXAPUState *d)
{
    int queued_bytes;
    qemu_spin_lock(&d->monitor.fifo_lock);
    queued_bytes = (int)fifo8_num_used(&d->monitor.fifo);
    qemu_spin_unlock(&d->monitor.fifo_lock);
    return queued_bytes;
}

static void throttle(MCPXAPUState *d)
{
    if (d->ep_frame_div % 8) {
        return;
    }

    if (d->monitor.fifo_capacity_bytes <= 0) {
        return;
    }

    int64_t start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int queued_bytes = monitor_num_used_bytes(d);

    while (!qatomic_read(&d->exiting) &&
           queued_bytes >= d->monitor.queued_bytes_high) {
        qemu_cond_timedwait(&d->cond, &d->lock, EP_FRAME_US / 1000);
        if (qatomic_read(&d->exiting)) {
            break;
        }
        queued_bytes = monitor_num_used_bytes(d);
    }

    if (queued_bytes > d->monitor.queued_bytes_low) {
        if (d->next_frame_time_us == 0 ||
            start_us - d->next_frame_time_us > EP_FRAME_US) {
            d->next_frame_time_us = start_us;
        }
        while (!qatomic_read(&d->exiting)) {
            int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
            if (remaining_ms > 0) {
                int sleep_ms = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
                qemu_cond_timedwait(&d->cond, &d->lock, sleep_ms);
            } else {
                break;
            }
        }
        d->next_frame_time_us += EP_FRAME_US;

        /* Nudge frame timing based on queue level to avoid drifting
         * toward one of the watermarks.
         */
        int mid = (d->monitor.queued_bytes_low + d->monitor.queued_bytes_high) / 2;
        d->next_frame_time_us += (queued_bytes > mid) - (queued_bytes < mid);
    } else {
        d->next_frame_time_us = start_us;
    }

    d->sleep_acc_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us;
}

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    /* A rudimentary calculation to determine approximately how taxed the APU
     * thread is, by measuring how much time we spend waiting for FIFO to drain
     * versus working on building frames.
     * =1: thread is not sleeping and likely falling behind realtime
     * <1: thread is able to complete work on time
     */
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (now - d->frame_count_time_ms >= 1000) {
        g_dbg.frames_processed = d->frame_count;
        float t = 1.0f - ((double)d->sleep_acc_us /
                          (double)((now - d->frame_count_time_ms) * 1000));
        g_dbg.utilization = t;

        d->frame_count_time_ms = now;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = { 0 };

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);

    if ((d->ep_frame_div + 1) % 8 == 0) {
#if 0
        FILE *fd = fopen("ep.pcm", "a+");
        assert(fd != NULL);
        fwrite(d->apu_fifo_output, sizeof(d->apu_fifo_output), 1, fd);
        fclose(fd);
#endif

        if (0 <= g_config.audio.volume_limit && g_config.audio.volume_limit < 1) {
            float f = pow(g_config.audio.volume_limit, M_E);
            for (int i = 0; i < 256; i++) {
                d->monitor.frame_buf[i][0] *= f;
                d->monitor.frame_buf[i][1] *= f;
            }
        }

        qemu_spin_lock(&d->monitor.fifo_lock);
        int num_bytes_free = (int)fifo8_num_free(&d->monitor.fifo);
        assert(num_bytes_free >= sizeof(d->monitor.frame_buf));
        fifo8_push_all(&d->monitor.fifo, (uint8_t *)d->monitor.frame_buf,
                       sizeof(d->monitor.frame_buf));
        qemu_spin_unlock(&d->monitor.fifo_lock);
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    }

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* Note: only supports millisecond resolution on Windows */
static void sleep_ns(int64_t ns)
{
#ifndef _WIN32
        struct timespec sleep_delay, rem_delay;
        sleep_delay.tv_sec = ns / 1000000000LL;
        sleep_delay.tv_nsec = ns % 1000000000LL;
        nanosleep(&sleep_delay, &rem_delay);
#else
        Sleep(ns / SCALE_MS);
#endif
}

static int getenv_int_clamped(const char *name, int min_value, int max_value,
                              int fallback)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }

    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return (int)parsed;
}

static void monitor_sink_cb(void *opaque, uint8_t *stream, int free_b)
{
    MCPXAPUState *s = MCPX_APU_DEVICE(opaque);

    if (!runstate_is_running()) {
        memset(stream, 0, free_b);
        return;
    }

    int avail;
    qemu_spin_lock(&s->monitor.fifo_lock);
    avail = fifo8_num_used(&s->monitor.fifo);
    qemu_spin_unlock(&s->monitor.fifo_lock);

    int to_copy = MIN(free_b, avail);
    int copied = 0;
    while (copied < to_copy) {
        uint32_t chunk_len;
        qemu_spin_lock(&s->monitor.fifo_lock);
        chunk_len = fifo8_pop_buf(&s->monitor.fifo, stream + copied,
                                  to_copy - copied);
        qemu_spin_unlock(&s->monitor.fifo_lock);
        if (!chunk_len) break;
        copied += chunk_len;
    }
    if (copied < free_b) {
        memset(stream + copied, 0, free_b - copied);
    }

    qemu_cond_broadcast(&s->cond);
}

static void monitor_init(MCPXAPUState *d)
{
    qemu_spin_init(&d->monitor.fifo_lock);

    int fifo_frames = 3;
    int audio_samples = 512;
#ifdef __ANDROID__
    fifo_frames = 48;
    audio_samples = 2048;
    fifo_frames = getenv_int_clamped("XEMU_ANDROID_AUDIO_FIFO_FRAMES", 3, 128,
                                     fifo_frames);
    audio_samples = getenv_int_clamped("XEMU_ANDROID_AUDIO_SAMPLES", 256, 4096,
                                       audio_samples);
#endif
    int fifo_capacity_bytes = fifo_frames * sizeof(d->monitor.frame_buf);
    fifo8_create(&d->monitor.fifo, fifo_capacity_bytes);
    d->monitor.fifo_capacity_bytes = fifo_capacity_bytes;

    struct SDL_AudioSpec sdl_audio_spec = {
        .freq = 48000,
        .format = AUDIO_S16SYS,
        .channels = 2,
        .samples = audio_samples,
        .callback = monitor_sink_cb,
        .userdata = d,
    };

    if (SDL_Init(SDL_INIT_AUDIO) < 0)  {
        fprintf(stderr, "Failed to initialize SDL audio subsystem: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_AudioDeviceID sdl_audio_dev;
    SDL_AudioSpec obtained_audio_spec;
    sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &sdl_audio_spec,
                                        &obtained_audio_spec,
                                        SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (sdl_audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        assert(!"SDL_OpenAudioDevice failed");
        exit(1);
    }

    int fifo_frame_bytes = sizeof(d->monitor.frame_buf);
    int drain_bytes = obtained_audio_spec.samples * 2 * sizeof(int16_t);
    if (drain_bytes <= 0) {
        drain_bytes = audio_samples * 2 * sizeof(int16_t);
    }
    drain_bytes = MAX(drain_bytes, fifo_frame_bytes);
    int max_high = MAX(d->monitor.fifo_capacity_bytes - fifo_frame_bytes,
                       fifo_frame_bytes);
    d->monitor.device_buffer_bytes = drain_bytes;
    d->monitor.queued_bytes_high = MIN(3 * drain_bytes, max_high);
    d->monitor.queued_bytes_low = MIN(drain_bytes, d->monitor.queued_bytes_high);

    SDL_PauseAudioDevice(sdl_audio_dev, 0);
}

static void mcpx_apu_realize(PCIDevice *dev, Error **errp)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(dev);

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init_io(&d->mmio, OBJECT(dev), &mcpx_apu_mmio_ops, d,
                          "mcpx-apu-mmio", 0x80000);

    memory_region_init_io(&d->vp.mmio, OBJECT(dev), &vp_ops, d,
                          "mcpx-apu-vp", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x20000, &d->vp.mmio);

    memory_region_init_io(&d->gp.mmio, OBJECT(dev), &gp_ops, d,
                          "mcpx-apu-gp", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x30000, &d->gp.mmio);

    memory_region_init_io(&d->ep.mmio, OBJECT(dev), &ep_ops, d,
                          "mcpx-apu-ep", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x50000, &d->ep.mmio);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);
}

static void mcpx_apu_exitfn(PCIDevice *dev)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(dev);
    d->exiting = true;
    qemu_cond_broadcast(&d->cond);
    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
}

static void mcpx_apu_reset(MCPXAPUState *d)
{
    qemu_mutex_lock(&d->lock); // FIXME: Can fail if thread is pegged, add flag
    memset(d->regs, 0, sizeof(d->regs));

    mcpx_apu_vp_reset(d);

    // FIXME: Reset DSP state
    memset(d->gp.dsp->core.pram_opcache, 0,
           sizeof(d->gp.dsp->core.pram_opcache));
    memset(d->ep.dsp->core.pram_opcache, 0,
           sizeof(d->ep.dsp->core.pram_opcache));
    d->set_irq = false;
    d->next_frame_time_us = 0;
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
}

// Note: This is handled as a VM state change and not as a `pre_save` callback
// because we want to halt the FIFO before any VM state is saved/restored to
// avoid corruption.
static void mcpx_apu_vm_state_change(void *opaque, bool running, RunState state)
{
    MCPXAPUState *d = opaque;

    if (state == RUN_STATE_SAVE_VM) {
        qemu_mutex_lock(&d->lock);
    }
}

static int mcpx_apu_post_save(void *opaque)
{
    MCPXAPUState *d = opaque;
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
    return 0;
}

static int mcpx_apu_pre_load(void *opaque)
{
    MCPXAPUState *d = opaque;
    mcpx_apu_reset(d);
    qemu_mutex_lock(&d->lock);
    return 0;
}

static int mcpx_apu_post_load(void *opaque, int version_id)
{
    MCPXAPUState *d = opaque;
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
    return 0;
}

static void mcpx_apu_reset_hold(Object *obj, ResetType type)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(obj);
    mcpx_apu_reset(d);
}

const VMStateDescription vmstate_vp_dsp_dma_state = {
    .name = "mcpx-apu/dsp-state/dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_UINT32(configuration, DSPDMAState),
        VMSTATE_UINT32(control, DSPDMAState),
        VMSTATE_UINT32(start_block, DSPDMAState),
        VMSTATE_UINT32(next_block, DSPDMAState),
        VMSTATE_BOOL(error, DSPDMAState),
        VMSTATE_BOOL(eol, DSPDMAState),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_vp_dsp_core_state = {
    .name = "mcpx-apu/dsp-state/core",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields      = (VMStateField[]) {
        // FIXME: Remove unnecessary fields
        VMSTATE_UINT16(instr_cycle, dsp_core_t),
        VMSTATE_UINT32(pc, dsp_core_t),
        VMSTATE_UINT32_ARRAY(registers, dsp_core_t, DSP_REG_MAX),
        VMSTATE_UINT32_2DARRAY(stack, dsp_core_t, 2, 16),
        VMSTATE_UINT32_ARRAY(xram, dsp_core_t, DSP_XRAM_SIZE),
        VMSTATE_UINT32_ARRAY(yram, dsp_core_t, DSP_YRAM_SIZE),
        VMSTATE_UINT32_ARRAY(pram, dsp_core_t, DSP_PRAM_SIZE),
        VMSTATE_UINT32_ARRAY(mixbuffer, dsp_core_t, DSP_MIXBUFFER_SIZE),
        VMSTATE_UINT32_ARRAY(periph, dsp_core_t, DSP_PERIPH_SIZE),
        VMSTATE_UINT32(loop_rep, dsp_core_t),
        VMSTATE_UINT32(pc_on_rep, dsp_core_t),
        VMSTATE_UINT16(interrupt_state, dsp_core_t),
        VMSTATE_UINT16(interrupt_instr_fetch, dsp_core_t),
        VMSTATE_UINT16(interrupt_save_pc, dsp_core_t),
        VMSTATE_UINT16(interrupt_counter, dsp_core_t),
        VMSTATE_UINT16(interrupt_ipl_to_raise, dsp_core_t),
        VMSTATE_UINT16(interrupt_pipeline_count, dsp_core_t),
        VMSTATE_INT16_ARRAY(interrupt_ipl, dsp_core_t, 12),
        VMSTATE_UINT16_ARRAY(interrupt_is_pending, dsp_core_t, 12),
        VMSTATE_UINT32(num_inst, dsp_core_t),
        VMSTATE_UINT32(cur_inst_len, dsp_core_t),
        VMSTATE_UINT32(cur_inst, dsp_core_t),
        VMSTATE_UNUSED(1),
        VMSTATE_UINT32(disasm_memory_ptr, dsp_core_t),
        VMSTATE_BOOL(exception_debugging, dsp_core_t),
        VMSTATE_UINT32(disasm_prev_inst_pc, dsp_core_t),
        VMSTATE_BOOL(disasm_is_looping, dsp_core_t),
        VMSTATE_UINT32(disasm_cur_inst, dsp_core_t),
        VMSTATE_UINT16(disasm_cur_inst_len, dsp_core_t),
        VMSTATE_UINT32_ARRAY(disasm_registers_save, dsp_core_t, 64),
// #ifdef DSP_DISASM_REG_PC
//         VMSTATE_UINT32(pc_save, dsp_core_t),
// #endif
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_vp_dsp_state = {
    .name = "mcpx-apu/dsp-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_STRUCT(core, DSPState, 1, vmstate_vp_dsp_core_state, dsp_core_t),
        VMSTATE_STRUCT(dma, DSPState, 1, vmstate_vp_dsp_dma_state, DSPDMAState),
        VMSTATE_INT32(save_cycles, DSPState),
        VMSTATE_UINT32(interrupts, DSPState),
        VMSTATE_END_OF_LIST()
    }
};


const VMStateDescription vmstate_vp_ssl_data = {
    .name = "mcpx_apu_voice_data",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(base, MCPXAPUVPSSLData, MCPX_HW_SSLS_PER_VOICE),
        VMSTATE_UINT8_ARRAY(count, MCPXAPUVPSSLData, MCPX_HW_SSLS_PER_VOICE),
        VMSTATE_INT32(ssl_index, MCPXAPUVPSSLData),
        VMSTATE_INT32(ssl_seg, MCPXAPUVPSSLData),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_mcpx_apu = {
    .name = "mcpx-apu",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_save = mcpx_apu_post_save,
    .pre_load = mcpx_apu_pre_load,
    .post_load = mcpx_apu_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, MCPXAPUState),
        VMSTATE_STRUCT_POINTER(gp.dsp, MCPXAPUState, vmstate_vp_dsp_state,
                               DSPState),
        VMSTATE_UINT32_ARRAY(gp.regs, MCPXAPUState, 0x10000),
        VMSTATE_STRUCT_POINTER(ep.dsp, MCPXAPUState, vmstate_vp_dsp_state,
                               DSPState),
        VMSTATE_UINT32_ARRAY(ep.regs, MCPXAPUState, 0x10000),
        VMSTATE_UINT32_ARRAY(regs, MCPXAPUState, 0x20000),
        VMSTATE_UINT32(vp.inbuf_sge_handle, MCPXAPUState),
        VMSTATE_UINT32(vp.outbuf_sge_handle, MCPXAPUState),
        VMSTATE_STRUCT_ARRAY(vp.ssl, MCPXAPUState, MCPX_HW_MAX_VOICES, 1,
                             vmstate_vp_ssl_data, MCPXAPUVPSSLData),
        VMSTATE_INT32(vp.ssl_base_page, MCPXAPUState),
        VMSTATE_UINT8_ARRAY(vp.hrtf_submix, MCPXAPUState, 4),
        VMSTATE_UINT8(vp.hrtf_headroom, MCPXAPUState),
        VMSTATE_UINT8_ARRAY(vp.submix_headroom, MCPXAPUState, NUM_MIXBINS),
        VMSTATE_UINT64_ARRAY(vp.voice_locked, MCPXAPUState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcpx_apu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = PCI_DEVICE_ID_NVIDIA_MCPX_APU;
    k->revision = 177;
    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    k->realize = mcpx_apu_realize;
    k->exit = mcpx_apu_exitfn;

    rc->phases.hold = mcpx_apu_reset_hold;

    dc->desc = "MCPX Audio Processing Unit";
    dc->vmsd = &vmstate_mcpx_apu;
}

static const TypeInfo mcpx_apu_info = {
    .name = "mcpx-apu",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(MCPXAPUState),
    .class_init = mcpx_apu_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            {},
        },
};

static void mcpx_apu_register(void)
{
    type_register_static(&mcpx_apu_info);
}
type_init(mcpx_apu_register);

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);
    while (!qatomic_read(&d->exiting)) {
        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
            d->set_irq = true;
        }

        if (d->set_irq) {
            qemu_mutex_unlock(&d->lock);
            bql_lock();
            update_irq(d);
            bql_unlock();
            qemu_mutex_lock(&d->lock);
            d->set_irq = false;
        }

        xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                            NV_PAPU_SECTL_XCNTMODE);
        fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
            qemu_cond_wait(&d->cond, &d->lock);
            continue;
        }
        throttle(d);
        se_frame((void *)d);
    }
    qemu_mutex_unlock(&d->lock);
    return NULL;
}

void mcpx_apu_init(PCIBus *bus, int devfn, MemoryRegion *ram)
{
    PCIDevice *dev = pci_create_simple(bus, devfn, "mcpx-apu");
    MCPXAPUState *d = MCPX_APU_DEVICE(dev);

    g_state = d;

    d->ram = ram;
    d->ram_ptr = memory_region_get_ram_ptr(d->ram);

    mcpx_apu_dsp_init(d);

    d->set_irq = false;
    d->exiting = false;

    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_add_vm_change_state_handler(mcpx_apu_vm_state_change, d);

    mcpx_apu_vp_init(d);
    monitor_init(d);
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread", mcpx_apu_frame_thread,
                       d, QEMU_THREAD_JOINABLE);
}
