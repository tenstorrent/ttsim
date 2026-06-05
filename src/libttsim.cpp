// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// libttsim.so exposes the device's physical I/O interfaces (e.g. PCIE, Ethernet).  These
// symbols form a stable, binary-compatible API intended for long-term use; changes should
// be rare and should always correspond to physical ports/connectivity on the device.
// The API/ABI contract and compatibility policy are documented in docs/libttsim_api.md.
#include "sim.h"
#include <algorithm>

#define API_EXPORT __attribute((visibility("default")))

#define BAR0_BASE 0x100000000ull // 4GiB
#define BAR2_BASE 0x120000000ull // 4GiB + 512MiB
#define BAR4_BASE 0x800000000ull // 32GiB

#define BAR0_SIZE (512 * 1024*1024)
#define BAR2_SIZE (1 * 1024*1024)
#if TT_ARCH_VERSION == 1
#define BAR4_SIZE (32ull * 1024*1024*1024) // 32GiB
#define REVISION_ID 0
#else
#define BAR4_SIZE (32 * 1024*1024)
#define REVISION_ID 1
#endif

#if TT_ARCH_VERSION == 0
// WH BAR4 is mapped to the top 32MB of BAR0
#define BAR4_SOC_BASE (BAR0_BASE + BAR0_SIZE - BAR4_SIZE)

#define ARC_CSM_BASE 0x1FE80000
#define ARC_CSM_LIMIT 0x1FEFFFFF
#define ARC_APB_BASE 0x1FF00000
#define ARC_APB_LIMIT 0x1FFFFFFF
#define ARC_COORD (0 | (10 << 6))
#define ARC_NOC_CSM_BASE 0x810000000ull
#define ARC_NOC_APB_BASE 0x880000000ull
#define IATU_BASE 0x1200ull
#define IATU_LIMIT 0x31FFull
#define IATU_REGION_ENABLE 0x80000000u
#define IATU_SUPPORTED_INBOUND_CTRL_2 0xC8000400u
#define IATU_SUPPORTED_INBOUND_TARGET 0x1E000000ull
#endif

static bool s_ttsim_running = false;
static bool s_ttsim_semihosting = false;
static void (*s_pfn_libttsim_pci_dma_mem_rd_bytes)(uint64_t paddr, void *p, uint32_t size);
static void (*s_pfn_libttsim_pci_dma_mem_wr_bytes)(uint64_t paddr, const void *p, uint32_t size);
#if TT_ARCH_VERSION == 0
static uint64_t s_tlb_cfg[186];

struct IatuRegion {
    uint32_t ctrl_1;
    uint32_t ctrl_2;
    uint32_t lower_base;
    uint32_t upper_base;
    uint32_t limit;
    uint32_t lower_target;
    uint32_t upper_target;
};
static IatuRegion s_iatu_outbound[16];
static IatuRegion s_iatu_inbound[16];

static void verify_iatu_region_write(uint32_t offset, uint32_t value) {
    uint32_t slot = offset / 0x100;
    TTSIM_VERIFY(slot < 32, UnimplementedFunctionality, "bar2: iATU offset=0x%x", offset);
    bool inbound = slot & 1;
    uint32_t region = slot >> 1;
    TTSIM_VERIFY(inbound, UnimplementedFunctionality, "bar2: outbound iATU region=%d", region);
    TTSIM_VERIFY(region == 1, UnimplementedFunctionality, "bar2: inbound iATU region=%d", region);
    uint32_t reg = offset % 0x100;
    switch (reg) {
        case 0x00:
            TTSIM_VERIFY(value == 0, UnimplementedFunctionality, "bar2: iATU ctrl_1=0x%x", value);
            return;
        case 0x04:
            TTSIM_VERIFY(!(value & IATU_REGION_ENABLE) || (value == IATU_SUPPORTED_INBOUND_CTRL_2),
                UnimplementedFunctionality, "bar2: inbound iATU ctrl_2=0x%x", value);
            return;
        case 0x14:
            TTSIM_VERIFY(value == uint32_t(IATU_SUPPORTED_INBOUND_TARGET), UnimplementedFunctionality,
                "bar2: inbound iATU lower_target=0x%x", value);
            return;
        case 0x18:
            TTSIM_VERIFY(value == uint32_t(IATU_SUPPORTED_INBOUND_TARGET >> 32), UnimplementedFunctionality,
                "bar2: inbound iATU upper_target=0x%x", value);
            return;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "bar2: iATU reg offset=0x%x", reg);
    }
}

static uint32_t *iatu_reg_field(uint32_t offset) {
    uint32_t slot = offset / 0x100;
    TTSIM_VERIFY(slot < 32, UnimplementedFunctionality, "bar2: iATU offset=0x%x", offset);
    uint32_t reg = offset % 0x100;
    IatuRegion *r = (slot & 1) ? &s_iatu_inbound[slot >> 1] : &s_iatu_outbound[slot >> 1];
    switch (reg) {
        case 0x00: return &r->ctrl_1;
        case 0x04: return &r->ctrl_2;
        case 0x08: return &r->lower_base;
        case 0x0C: return &r->upper_base;
        case 0x10: return &r->limit;
        case 0x14: return &r->lower_target;
        case 0x18: return &r->upper_target;
        default: TTSIM_ERROR(UnimplementedFunctionality, "bar2: iATU reg offset=0x%x", reg);
    }
}
#elif TT_ARCH_VERSION == 1
static uint32_t s_tlb_cfg[210*3];
#endif

extern "C" API_EXPORT void libttsim_init() {
    TTSIM_VERIFY(!s_ttsim_running, ConfigurationError, "sim is already running");
    if (char *s = getenv("TTSIM_SEMIHOSTING")) {
        TTSIM_VERIFY(!strcmp(s, "1"), ConfigurationError, "TTSIM_SEMIHOSTING must be set to 1");
        s_ttsim_semihosting = true;
    }
    ttsim_init();
    s_ttsim_running = true;
}

extern "C" API_EXPORT void libttsim_exit() {
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    ttsim_exit();
    s_ttsim_running = false;
}

extern "C" API_EXPORT void libttsim_set_pci_dma_mem_callbacks(
    decltype(s_pfn_libttsim_pci_dma_mem_rd_bytes) pci_dma_mem_rd,
    decltype(s_pfn_libttsim_pci_dma_mem_wr_bytes) pci_dma_mem_wr
) {
    TTSIM_VERIFY(!s_ttsim_running, ConfigurationError, "sim is already running");
    s_pfn_libttsim_pci_dma_mem_rd_bytes = pci_dma_mem_rd;
    s_pfn_libttsim_pci_dma_mem_wr_bytes = pci_dma_mem_wr;
}

static uint32_t pci_config_rd32(uint32_t offset) {
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case 0x0: return 0x1E52 | (0x401E << 16); // vendor ID, device ID
#elif TT_ARCH_VERSION == 1
        case 0x0: return 0x1E52 | (0xB140 << 16); // vendor ID, device ID
#endif
        case 0x08: return 0x12000000 | REVISION_ID;  // class code, subclass, prog-if, revision ID
        case 0x10: return 0x4 | uint32_t(BAR0_BASE); // 64-bit, not prefetchable (?), low address bits
        case 0x14: return uint32_t(BAR0_BASE >> 32); // high address bits
        case 0x18: return 0x4 | uint32_t(BAR2_BASE); // 64-bit, not prefetchable (?), low address bits
        case 0x1C: return uint32_t(BAR2_BASE >> 32); // high address bits
        case 0x20: return 0x4 | uint32_t(BAR4_BASE); // 64-bit, not prefetchable (?), low address bits
        case 0x24: return uint32_t(BAR4_BASE >> 32); // high address bits
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

extern "C" API_EXPORT uint32_t libttsim_pci_config_rd32(uint32_t bus_device_function, uint32_t offset) {
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(!bus_device_function, UndefinedBehavior, "bus_device_function=0x%x", bus_device_function);
    TTSIM_VERIFY(!(offset & 3), UndefinedBehavior, "misaligned offset=0x%x", offset);
    return pci_config_rd32(offset);
}

extern "C" API_EXPORT void libttsim_pci_config_wr32(uint32_t bus_device_function, uint32_t offset, uint32_t data) {
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(!bus_device_function, UndefinedBehavior, "bus_device_function=0x%x", bus_device_function);
    TTSIM_VERIFY(!(offset & 3), UndefinedBehavior, "misaligned offset=0x%x", offset);
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
}

static std::pair<uint32_t, uint64_t> tlb_translate(uint32_t offset, uint32_t size) {
#if TT_ARCH_VERSION == 0
    uint32_t tlb_index, window_bits;
    if (offset < 0x9C00000) {
        tlb_index = offset / 0x100000;
        window_bits = 20; // 1 MiB TLBs (indices 0-155)
    } else if (offset < 0xB000000) {
        tlb_index = ((offset - 0x9C00000) / 0x200000) + 156;
        window_bits = 21; // 2 MiB TLBs (indices 156-165)
    } else {
        tlb_index = ((offset - 0xB000000) / 0x1000000) + 166;
        window_bits = 24; // 16 MiB TLBs (indices 166-185)
    }
    uint32_t window_mask = (1 << window_bits) - 1;
    offset &= window_mask;
    uint32_t n_addr_bits = 36 - window_bits; // number of local_offset bits in TLB config
    TTSIM_VERIFY(tlb_index < std::size(s_tlb_cfg), AssertionFailure, "tlb_index=%d", tlb_index);
    TTSIM_VERIFY(offset + size - 1 <= window_mask, UnsupportedFunctionality, "TLB region overrun: offset=0x%x size=%d", offset, size);
    uint64_t tlb_cfg = s_tlb_cfg[tlb_index];
    uint64_t addr_bits = tlb_cfg & ((1ull << n_addr_bits) - 1);
    uint64_t addr = (addr_bits << window_bits) | offset;
    tlb_cfg >>= n_addr_bits; // after this shift, bit positions correspond directly to the spec's "first bit" column relative to N
    TTSIM_VERIFY(!(tlb_cfg & ~uint64_t(0x2C000FFF)), UnimplementedFunctionality, "tlb_cfg=0x%llx", tlb_cfg);
    uint32_t coord = tlb_cfg & 0xFFF;
    uint32_t ordering = bits<27,26>(tlb_cfg);
    TTSIM_VERIFY(ordering != 3, UndefinedBehavior, "tlb_cfg ordering=%d", ordering);
#elif TT_ARCH_VERSION == 1
    uint32_t tlb_index = offset / 0x200000;
    offset &= 0x1FFFFF;
    TTSIM_VERIFY(3*tlb_index < std::size(s_tlb_cfg), AssertionFailure, "tlb_index=%d", tlb_index);
    TTSIM_VERIFY(offset + size - 1 <= 0x1FFFFF, UnsupportedFunctionality, "TLB region overrun: offset=0x%x size=%d", offset, size);
    uint32_t tlb_cfg0 = s_tlb_cfg[3*tlb_index + 0];
    uint32_t tlb_cfg1 = s_tlb_cfg[3*tlb_index + 1];
    uint32_t tlb_cfg2 = s_tlb_cfg[3*tlb_index + 2];
    TTSIM_VERIFY(tlb_cfg1 <= 0x7FFFFF, UnimplementedFunctionality, "tlb_cfg1=0x%x", tlb_cfg1);
    TTSIM_VERIFY(!(tlb_cfg2 & ~0xC0u), UnimplementedFunctionality, "tlb_cfg2=0x%x", tlb_cfg2);
    uint32_t ordering = bits<7,6>(tlb_cfg2);
    TTSIM_VERIFY(ordering != 3, UnimplementedFunctionality, "tlb_cfg ordering=%d", ordering);
    uint64_t addr_bits = tlb_cfg0 | (uint64_t(tlb_cfg1 & 0x7FF) << 32);
    uint64_t addr = (uint64_t(addr_bits) << 21) | offset;
    uint32_t coord = bits<22,11>(tlb_cfg1);
#endif
    coord = remap_virtual_coordinate(0, coord);
    return {coord, addr};
}

#if TT_ARCH_VERSION == 1
static std::pair<uint32_t, uint64_t> tlb_translate_bar4(uint64_t offset, uint32_t size) {
    uint32_t tlb_index = 202 + uint32_t(offset >> 32);
    uint32_t window_offset = uint32_t(offset);
    TTSIM_VERIFY(3*tlb_index < std::size(s_tlb_cfg), AssertionFailure, "tlb_index=%d", tlb_index);
    TTSIM_VERIFY(uint64_t(window_offset) + size <= 0x100000000ull, UnsupportedFunctionality, "TLB region overrun: offset=0x%x size=%d", window_offset, size);
    uint32_t tlb_cfg0 = s_tlb_cfg[3*tlb_index + 0];
    uint32_t tlb_cfg1 = s_tlb_cfg[3*tlb_index + 1];
    uint32_t tlb_cfg2 = s_tlb_cfg[3*tlb_index + 2];
    TTSIM_VERIFY(!(tlb_cfg1 & ~0x18000FFFu), UnimplementedFunctionality, "tlb_cfg1=0x%x", tlb_cfg1);
    uint32_t ordering = bits<28,27>(tlb_cfg1);
    TTSIM_VERIFY(ordering != 3, UnimplementedFunctionality, "tlb_cfg ordering=%d", ordering);
    TTSIM_VERIFY(!tlb_cfg2, UnimplementedFunctionality, "tlb_cfg2=0x%x", tlb_cfg2);
    uint64_t addr = (uint64_t(tlb_cfg0) << 32) | window_offset;
    uint32_t coord = bits<11,0>(tlb_cfg1);
    coord = remap_virtual_coordinate(0, coord);
    return {coord, addr};
}
#endif

extern "C" API_EXPORT void libttsim_pci_mem_rd_bytes(uint64_t paddr, void *p, uint32_t size) {
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(size, UndefinedBehavior, "size=%d", size);
    switch (paddr) {
        case BAR0_BASE ... BAR0_BASE + BAR0_SIZE - 1: {
            uint32_t offset = paddr - BAR0_BASE;
            TTSIM_VERIFY(offset + size <= BAR0_SIZE, UndefinedBehavior, "bar0 overrun: offset=0x%x size=%d", offset, size);
            switch (offset) {
#if TT_ARCH_VERSION == 0
                case 0x00000000 ... 0x1EFFFFFF:
#elif TT_ARCH_VERSION == 1
                case 0x00000000 ... 0x193FFFFF:
#endif
                {
                    auto [coord, addr] = tlb_translate(offset, size);
                    tile_rd_bytes(coord, addr, p, size);
                    break;
                }
#if TT_ARCH_VERSION == 0
                case ARC_CSM_BASE ... ARC_CSM_LIMIT: {
                    uint32_t csm_offset = offset - ARC_CSM_BASE;
                    tile_rd_bytes(ARC_COORD, ARC_NOC_CSM_BASE + csm_offset, p, size);
                    break;
                }
                case ARC_APB_BASE ... ARC_APB_LIMIT: {
                    uint32_t apb_offset = offset - ARC_APB_BASE;
                    tile_rd_bytes(ARC_COORD, ARC_NOC_APB_BASE + apb_offset, p, size);
                    break;
                }
#endif
                default:
                    TTSIM_ERROR(UnimplementedFunctionality, "bar0: offset=0x%x size=%d", offset, size);
            }
            break;
        }
        case BAR2_BASE ... BAR2_BASE + BAR2_SIZE - 1: {
#if TT_ARCH_VERSION == 0
            TTSIM_VERIFY(size == 4, UnsupportedFunctionality, "bar2: size=%d", size);
            TTSIM_VERIFY(!(paddr & 3), UnsupportedFunctionality, "bar2: misaligned paddr=0x%llx", paddr);
            uint32_t offset = paddr - BAR2_BASE;
            switch (offset) {
                case IATU_BASE ... IATU_LIMIT:
                    mem_wr<uint32_t>(p, *iatu_reg_field(offset - IATU_BASE));
                    break;
                default:
                    TTSIM_ERROR(UnimplementedFunctionality, "bar2: offset=0x%x size=%d", offset, size);
            }
            break;
#else
            TTSIM_ERROR(UnimplementedFunctionality, "bar2: paddr=0x%llx size=%d", paddr, size);
#endif
        }
        case BAR4_BASE ... BAR4_BASE + BAR4_SIZE - 1: {
            uint64_t offset = paddr - BAR4_BASE;
            TTSIM_VERIFY(offset + size <= BAR4_SIZE, UndefinedBehavior, "bar4 overrun: offset=0x%llx size=%d", offset, size);
#if TT_ARCH_VERSION == 0
            uint64_t soc_addr = BAR4_SOC_BASE + offset;
            libttsim_pci_mem_rd_bytes(soc_addr, p, size);
#elif TT_ARCH_VERSION == 1
            auto [coord, addr] = tlb_translate_bar4(offset, size);
            tile_rd_bytes(coord, addr, p, size);
#else
            TTSIM_ERROR(UnimplementedFunctionality, "bar4: paddr=0x%llx size=%d", paddr, size);
#endif
            break;
        }
        default:
            TTSIM_ERROR(UndefinedBehavior, "paddr=0x%llx size=%d", paddr, size);
    }
}

#if TT_ARCH_VERSION == 0
static void tlb_cfg_wr64(uint32_t tlb_offset, uint64_t data) {
    uint32_t index = tlb_offset / 8;
    TTSIM_VERIFY(index < std::size(s_tlb_cfg), AssertionFailure, "index=%d", index);
    s_tlb_cfg[index] = data;
}

static void tlb_cfg_wr32(uint32_t tlb_offset, uint32_t data) {
    uint32_t index = tlb_offset / 8;
    TTSIM_VERIFY(index < std::size(s_tlb_cfg), AssertionFailure, "index=%d", index);
    if (tlb_offset & 4) {
        s_tlb_cfg[index] = (s_tlb_cfg[index] & 0xFFFFFFFFull) | (uint64_t(data) << 32);
    } else {
        s_tlb_cfg[index] = (s_tlb_cfg[index] & 0xFFFFFFFF00000000ull) | data;
    }
}
#elif TT_ARCH_VERSION == 1
static void tlb_cfg_wr32(uint32_t index, uint32_t data) {
    TTSIM_VERIFY(index < std::size(s_tlb_cfg), AssertionFailure, "index=%d", index);
    s_tlb_cfg[index] = data;
}
#endif

extern "C" API_EXPORT void libttsim_pci_mem_wr_bytes(uint64_t paddr, const void *p, uint32_t size) {
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(size, UndefinedBehavior, "size=%d", size);
    switch (paddr) {
        case BAR0_BASE ... BAR0_BASE + BAR0_SIZE - 1: {
            uint32_t offset = paddr - BAR0_BASE;
            TTSIM_VERIFY(offset + size <= BAR0_SIZE, UndefinedBehavior, "bar0 overrun: offset=0x%x size=%d", offset, size);
            switch (offset) {
#if TT_ARCH_VERSION == 0
                case 0x00000000 ... 0x1EFFFFFF:
#elif TT_ARCH_VERSION == 1
                case 0x00000000 ... 0x193FFFFF:
#endif
                {
                    auto [coord, addr] = tlb_translate(offset, size);
                    tile_wr_bytes(coord, addr, p, size);
                    break;
                }
#if TT_ARCH_VERSION == 0
                case 0x1FC00000 ... 0x1FC005CF: {
                    TTSIM_VERIFY(!(offset & (size - 1)), UnsupportedFunctionality, "bar0: misaligned offset=0x%x size=%d", offset, size);
                    uint32_t tlb_offset = offset - 0x1FC00000;
                    if (size == 8) {
                        tlb_cfg_wr64(tlb_offset, mem_rd<uint64_t>(p));
                    } else if (size == 4) {
                        tlb_cfg_wr32(tlb_offset, mem_rd<uint32_t>(p));
                    } else {
                        TTSIM_ERROR(UnsupportedFunctionality, "tlb_cfg: size=%d", size);
                    }
                    break;
                }
                case ARC_CSM_BASE ... ARC_CSM_LIMIT: {
                    uint32_t csm_offset = offset - ARC_CSM_BASE;
                    tile_wr_bytes(ARC_COORD, ARC_NOC_CSM_BASE + csm_offset, p, size);
                    break;
                }
                case ARC_APB_BASE ... ARC_APB_LIMIT: {
                    uint32_t apb_offset = offset - ARC_APB_BASE;
                    tile_wr_bytes(ARC_COORD, ARC_NOC_APB_BASE + apb_offset, p, size);
                    break;
                }
#elif TT_ARCH_VERSION == 1
                case 0x1FC00000 ... 0x1FC009D4:
                    TTSIM_VERIFY(!(offset & 3), UnsupportedFunctionality, "bar0: misaligned offset=0x%x", offset);
                    TTSIM_VERIFY(size == 4, UnsupportedFunctionality, "bar0: offset=0x%x size=%d", offset, size);
                    tlb_cfg_wr32((offset - 0x1FC00000) / 4, mem_rd<uint32_t>(p));
                    break;
#endif
                default:
                    TTSIM_ERROR(UnimplementedFunctionality, "bar0: offset=0x%x size=%d", offset, size);
            }
            break;
        }
        case BAR2_BASE ... BAR2_BASE + BAR2_SIZE - 1: {
#if TT_ARCH_VERSION == 0
            TTSIM_VERIFY(size == 4, UnsupportedFunctionality, "bar2: size=%d", size);
            TTSIM_VERIFY(!(paddr & 3), UnsupportedFunctionality, "bar2: misaligned paddr=0x%llx", paddr);
            uint32_t offset = paddr - BAR2_BASE;
            switch (offset) {
                case IATU_BASE ... IATU_LIMIT: {
                    uint32_t iatu_offset = offset - IATU_BASE;
                    uint32_t value = mem_rd<uint32_t>(p);
                    verify_iatu_region_write(iatu_offset, value);
                    *iatu_reg_field(iatu_offset) = value;
                    break;
                }
                default:
                    TTSIM_ERROR(UnimplementedFunctionality, "bar2: offset=0x%x size=%d", offset, size);
            }
            break;
#else
            TTSIM_ERROR(UnimplementedFunctionality, "bar2: paddr=0x%llx size=%d", paddr, size);
#endif
        }
        case BAR4_BASE ... BAR4_BASE + BAR4_SIZE - 1: {
            uint64_t offset = paddr - BAR4_BASE;
            TTSIM_VERIFY(offset + size <= BAR4_SIZE, UndefinedBehavior, "bar4 overrun: offset=0x%llx size=%d", offset, size);
#if TT_ARCH_VERSION == 0
            uint64_t soc_addr = BAR4_SOC_BASE + offset;
            libttsim_pci_mem_wr_bytes(soc_addr, p, size);
#elif TT_ARCH_VERSION == 1
            auto [coord, addr] = tlb_translate_bar4(offset, size);
            tile_wr_bytes(coord, addr, p, size);
#else
            TTSIM_ERROR(UnimplementedFunctionality, "bar4: paddr=0x%llx size=%d", paddr, size);
#endif
            break;
        }
        default:
            TTSIM_ERROR(UndefinedBehavior, "paddr=0x%llx size=%d", paddr, size);
    }
}

void libttsim_pci_dma_mem_rd_bytes(uint64_t paddr, void *p, uint32_t size) {
    TTSIM_VERIFY(s_pfn_libttsim_pci_dma_mem_rd_bytes, ConfigurationError, "DMA read callback not installed");
    s_pfn_libttsim_pci_dma_mem_rd_bytes(paddr, p, size);
}

void libttsim_pci_dma_mem_wr_bytes(uint64_t paddr, const void *p, uint32_t size) {
    TTSIM_VERIFY(s_pfn_libttsim_pci_dma_mem_wr_bytes, ConfigurationError, "DMA write callback not installed");
    s_pfn_libttsim_pci_dma_mem_wr_bytes(paddr, p, size);
}

// transitional; to be removed once the PCIe/BAR path is in place everywhere (see docs/libttsim_api.md)
extern "C" API_EXPORT void libttsim_tile_rd_bytes(uint32_t x, uint32_t y, uint64_t addr, void *p, uint32_t size) {
    TTSIM_ERROR_NOFMT(UnsupportedFunctionality);
}

// transitional; to be removed once the PCIe/BAR path is in place everywhere (see docs/libttsim_api.md)
extern "C" API_EXPORT void libttsim_tile_wr_bytes(uint32_t x, uint32_t y, uint64_t addr, const void *p, uint32_t size) {
    TTSIM_ERROR_NOFMT(UnsupportedFunctionality);
}

extern "C" API_EXPORT void libttsim_clock(uint32_t n_clocks) {
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    for (uint32_t i = 0; i < n_clocks; i++) {
        for (uint32_t tile_id = 0; tile_id < std::size(g_t_tiles); tile_id++) {
            TensixTile *p_tile = &g_t_tiles[tile_id];
            // Hack: for simulator perf, we clock a tile repeatedly as long as it successfully executes at least one Tensix instruction
            for (;;) {
                for (uint32_t rv32_mask = p_tile->rv32_cores_active; rv32_mask; rv32_mask &= rv32_mask-1) {
                    uint32_t rv32_index = __builtin_ctz(rv32_mask);
                    rv32_step(&p_tile->rv32[rv32_index]);
                }
                [[maybe_unused]] bool any_tensix = false;
                for (uint32_t tensix_id = 0; tensix_id < std::size(p_tile->tensix); tensix_id++) {
                    TensixState *p_tensix = &p_tile->tensix[tensix_id];
                    for (uint32_t pipe_mask = p_tensix->inst_pipes_active; pipe_mask; pipe_mask &= pipe_mask-1) {
                        uint32_t pipe = __builtin_ctz(pipe_mask);
                        uint32_t inst_rd_ptr = p_tensix->inst_rd_ptr[pipe];
                        if (inst_rd_ptr != p_tensix->inst_wr_ptr[pipe]) {
                            uint32_t inst = p_tensix->inst[pipe][inst_rd_ptr];
                            if (tensix_decode_and_execute(p_tensix, pipe, inst)) {
                                any_tensix = true;
                                inst_rd_ptr = (inst_rd_ptr + 1) % TENSIX_INST_FIFO_SIZE;
                                p_tensix->inst_rd_ptr[pipe] = inst_rd_ptr;
                                if (inst_rd_ptr == p_tensix->inst_wr_ptr[pipe]) {
                                    p_tensix->inst_pipes_active &= ~(1 << pipe);
                                }
                            }
                        }
                    }
                }
                if (!any_tensix) {
                    break; // no Tensix instructions executed, bail out
                }
            }
        }
        for (uint64_t rv32_mask = g_rv32_cores_active; rv32_mask; rv32_mask &= rv32_mask-1) {
            uint32_t core_index = __builtin_ctzll(rv32_mask);
            uint32_t tile_id = core_index / RV32_CORES_PER_E_TILE;
            uint32_t rv32_index = core_index % RV32_CORES_PER_E_TILE;
            rv32_step(&g_e_tiles[tile_id].rv32[rv32_index]);
        }
        g_clock++;
    }
}

// Simplified read/write APIs for semihosting -- not fully general support for the address map
static uint8_t syscall_mem_rd(uint32_t tile_id, uint32_t riscv_id, uint64_t addr) {
    if (addr < sizeof(g_t_tiles[tile_id].sram)) {
        return g_t_tiles[tile_id].sram[addr];
    } else if (!(riscv_id & 0x80000000) && (addr >= RISCV_LOCAL_MEM_BASE) && (addr < RISCV_LOCAL_MEM_BASE + 0x1000)) {
        return g_t_tiles[tile_id].rv32_local_ram[0][addr - RISCV_LOCAL_MEM_BASE];
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

static void syscall_mem_wr(uint32_t tile_id, uint32_t riscv_id, uint64_t addr, uint8_t data) {
    if (addr < sizeof(g_t_tiles[tile_id].sram)) {
        g_t_tiles[tile_id].sram[addr] = data;
    } else if (!(riscv_id & 0x80000000) && (addr >= RISCV_LOCAL_MEM_BASE) && (addr < RISCV_LOCAL_MEM_BASE + 0x1000)) {
        g_t_tiles[tile_id].rv32_local_ram[0][addr - RISCV_LOCAL_MEM_BASE] = data;
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

static uint64_t sys_close(uint32_t tile_id, uint32_t riscv_id, uint64_t fd) {
    TTSIM_VERIFY(fd <= 2, UnimplementedFunctionality, "fd=%lld", fd); // only support std[in,out,err] and ignore attempts to close them for now
    return 0;
}

static uint64_t sys_write(uint32_t tile_id, uint32_t riscv_id, uint64_t fd, uint64_t buf, uint64_t count) {
    TTSIM_VERIFY((fd == 1) || (fd == 2), UnimplementedFunctionality, "fd=%lld", fd);
    FILE *f = (fd == 1) ? stdout : stderr;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t byte = syscall_mem_rd(tile_id, riscv_id, buf + i);
        fwrite(&byte, 1, 1, f);
    }
    return count;
}

static uint64_t sys_fstat(uint32_t tile_id, uint32_t riscv_id, uint64_t fd, uint64_t p_statbuf) {
    TTSIM_VERIFY(fd == 1, UnimplementedFunctionality, "fd=%lld", fd);
    for (uint32_t offset = 0; offset < 112; offset++) {
        syscall_mem_wr(tile_id, riscv_id, p_statbuf + offset, 0);
    }
    return 0;
}

static uint64_t sys_exit(uint32_t tile_id, uint32_t riscv_id, uint64_t status) {
    TTSIM_VERIFY(status <= 255, UndefinedBehavior, "exit(%lld)", status);
    // note that we skip ttsim_exit() here, so that semihosting apps don't print anything extra to the console
    // XXX may eventually need a way to dump stats here w/o heartbeat
    _Exit(status);
}

static uint64_t sys_brk(uint32_t tile_id, uint32_t riscv_id, uint64_t addr) {
    return 0; // no heap support yet: return 0 unconditionally to indicate no heap is available
}

// see libgloss/riscv/machine/syscall.h in newlib
uint64_t libttsim_syscall(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t syscall, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    TTSIM_VERIFY(s_ttsim_semihosting, ConfigurationError, "semihosting is not enabled");
    TTSIM_VERIFY(tile_type == 'T', UnsupportedFunctionality, "tile_type=%c", tile_type);
    TTSIM_VERIFY((riscv_id == 0) || (riscv_id == 0x80000000), UnsupportedFunctionality, "riscv_id=0x%x", riscv_id);
    switch (syscall) {
        case 57: return sys_close(tile_id, riscv_id, arg0);
        case 64: return sys_write(tile_id, riscv_id, arg0, arg1, arg2);
        case 80: return sys_fstat(tile_id, riscv_id, arg0, arg1);
        case 93: return sys_exit(tile_id, riscv_id, arg0);
        case 214: return sys_brk(tile_id, riscv_id, arg0);
        default: TTSIM_ERROR(UnimplementedFunctionality, "syscall=%lld", syscall);
    }
}
