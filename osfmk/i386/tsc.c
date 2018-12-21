/*
 * Copyright (c) 2005-2007 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */

/*
 *	File:		i386/tsc.c
 *	Purpose:	Initializes the TSC and the various conversion
 *			factors needed by other parts of the system.
 */


#include <mach/mach_types.h>

#include <kern/cpu_data.h>
#include <kern/cpu_number.h>
#include <kern/clock.h>
#include <kern/host_notify.h>
#include <kern/macro_help.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <kern/assert.h>
#include <mach/vm_prot.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>		/* for kernel_map */
#include <architecture/i386/pio.h>
#include <i386/machine_cpu.h>
#include <i386/cpuid.h>
#include <i386/mp.h>
#include <i386/machine_routines.h>
#include <i386/proc_reg.h>
#include <i386/tsc.h>
#include <i386/misc_protos.h>
#include <pexpert/pexpert.h>
#include <machine/limits.h>
#include <machine/commpage.h>
#include <sys/kdebug.h>
#include <pexpert/device_tree.h>

uint64_t	busFCvtt2n = 0;
uint64_t	busFCvtn2t = 0;
uint64_t	tscFreq = 0;
uint64_t	tscFCvtt2n = 0;
uint64_t	tscFCvtn2t = 0;
uint64_t	tscGranularity = 0;
uint64_t	bus2tsc = 0;
uint64_t	busFreq = 0;
uint32_t	flex_ratio = 0;
uint32_t	flex_ratio_min = 0;
uint32_t	flex_ratio_max = 0;

uint64_t	tsc_at_boot = 0;

#define bit(n)		(1ULL << (n))
#define bitmask(h,l)	((bit(h)|(bit(h)-1)) & ~(bit(l)-1))
#define bitfield(x,h,l)	(((x) & bitmask(h,l)) >> l)

/* Decimal powers: */
#define kilo (1000ULL)
#define Mega (kilo * kilo)
#define Giga (kilo * Mega)
#define Tera (kilo * Giga)
#define Peta (kilo * Tera)

#define CPU_FAMILY_PENTIUM_M	(0x6)

static const char	FSB_Frequency_prop[] = "FSBFrequency";
static const char   	FSB_CPUFrequency_prop[] = "CPUFrequency";
static const char	TSC_at_boot_prop[]   = "InitialTSC";

/*
 * This routine extracts a frequency property in Hz from the device tree.
 * Also reads any initial TSC value at boot from the device tree.
 */
static uint64_t
EFI_get_frequency(const char *prop)
{
	uint64_t	frequency = 0;
	DTEntry		entry;
	void		*value;
	unsigned int	size;

	if (DTLookupEntry(0, "/efi/platform", &entry) != kSuccess) {
		kprintf("EFI_get_frequency: didn't find /efi/platform\n");
		return 0;
	}
	if (DTGetProperty(entry,prop,&value,&size) != kSuccess) {
		kprintf("EFI_get_frequency: property %s not found\n", prop);
		return 0;
	}
	if (size == sizeof(uint64_t)) {
		frequency = *(uint64_t *) value;
		kprintf("EFI_get_frequency: read %s value: %llu\n",
			prop, frequency);
	}

	/*
	 * While we're here, see if EFI published an initial TSC value.
	 */
	if (DTGetProperty(entry,"InitialTSC",&value,&size) == kSuccess) {
		if (size == sizeof(uint64_t)) {
			tsc_at_boot = *(uint64_t *) value;
			kprintf("EFI_get_frequency: read InitialTSC: %llu\n",
				tsc_at_boot);
		}
	}

	return frequency;
}

/*** for AMD CPU from AnV 10.9.2 ***/
static uint64_t
EFI_CPU_Frequency(void)
{
    uint64_t    frequency = 0;

    DTEntry     entry;
    void        *value;
    unsigned int    size;

    if (DTLookupEntry(0, "/efi/platform", &entry) != kSuccess) {
        kprintf("EFI_CPU_Frequency: didn't find /efi/platform\n");
        return 0;
    }
    if (DTGetProperty(entry,FSB_CPUFrequency_prop,&value,&size) != kSuccess) {
        kprintf("EFI_CPU_Frequency: property %s not found\n",
                FSB_Frequency_prop);
        return 0;
    }
    if (size == sizeof(uint64_t)) {
        frequency = *(uint64_t *) value;
        kprintf("EFI_CPU_Frequency: read %s value: %llu\n",
                FSB_Frequency_prop, frequency);
        if (!(10*Mega < frequency && frequency < 50*Giga)) {
            kprintf("EFI_Fake_MSR: value out of range\n");
            frequency = 0;
        }
    } else {
        kprintf("EFI_CPU_Frequency: unexpected size %d\n", size);
    }

    return frequency;
}

/*
 * Initialize the various conversion factors needed by code referencing
 * the TSC.
 */
void
tsc_init(void)
{
	boolean_t	N_by_2_bus_ratio = FALSE;

    if (cpuid_vmm_present()) {
        kprintf("VMM vendor %u TSC frequency %u KHz bus frequency %u KHz\n",
                cpuid_vmm_info()->cpuid_vmm_family,
                cpuid_vmm_info()->cpuid_vmm_tsc_frequency,
                cpuid_vmm_info()->cpuid_vmm_bus_frequency);

        if (cpuid_vmm_info()->cpuid_vmm_tsc_frequency &&
            cpuid_vmm_info()->cpuid_vmm_bus_frequency) {

            busFreq = (uint64_t)cpuid_vmm_info()->cpuid_vmm_bus_frequency * kilo;
            busFCvtt2n = ((1 * Giga) << 32) / busFreq;
            busFCvtn2t = 0xFFFFFFFFFFFFFFFFULL / busFCvtt2n;
			
            tscFreq = (uint64_t)cpuid_vmm_info()->cpuid_vmm_tsc_frequency * kilo;
            tscFCvtt2n = ((1 * Giga) << 32) / tscFreq;
            tscFCvtn2t = 0xFFFFFFFFFFFFFFFFULL / tscFCvtt2n;
			
            tscGranularity = tscFreq / busFreq;
			
            bus2tsc = tmrCvt(busFCvtt2n, tscFCvtn2t);

            return;
        }
    }

    if (IsIntelCPU()){
    	switch (cpuid_cpufamily()) {
	case CPUFAMILY_INTEL_KABYLAKE:
	case CPUFAMILY_INTEL_SKYLAKE: {
		/*
                * SkyLake and later has an Always Running Timer (ART) providing
		 * the reference frequency. CPUID leaf 0x15 determines the
		 * rationship between this and the TSC frequency expressed as
		 *   -	multiplier (numerator, N), and 
		 *   -	divisor (denominator, M).
		 * So that TSC = ART * N / M.
		 */
		cpuid_tsc_leaf_t *tsc_leafp = &cpuid_info()->cpuid_tsc_leaf;
		uint64_t	 N = (uint64_t) tsc_leafp->numerator;
		uint64_t	 M = (uint64_t) tsc_leafp->denominator;
		uint64_t	 refFreq;

		refFreq = EFI_get_frequency("ARTFrequency");
		if (refFreq == 0)
			refFreq = BASE_ART_CLOCK_SOURCE;

		assert(N != 0);
		assert(M != 1);
		tscFreq = refFreq * N / M;
		busFreq = tscFreq;		/* bus is APIC frequency */

		kprintf(" ART: Frequency = %6d.%06dMHz, N/M = %lld/%llu\n",
			(uint32_t)(refFreq / Mega),
			(uint32_t)(refFreq % Mega), 
			N, M);

		break;
	    }
	default: {
		uint64_t msr_flex_ratio;
		uint64_t msr_platform_info;

		/* See if FLEX_RATIO is being used */
		msr_flex_ratio = rdmsr64(MSR_FLEX_RATIO);
		msr_platform_info = rdmsr64(MSR_PLATFORM_INFO);
		flex_ratio_min = (uint32_t)bitfield(msr_platform_info, 47, 40);
		flex_ratio_max = (uint32_t)bitfield(msr_platform_info, 15, 8);
		/* No BIOS-programed flex ratio. Use hardware max as default */
		tscGranularity = flex_ratio_max;
		if (msr_flex_ratio & bit(16)) {
		 	/* Flex Enabled: Use this MSR if less than max */
			flex_ratio = (uint32_t)bitfield(msr_flex_ratio, 15, 8);
			if (flex_ratio < flex_ratio_max)
				tscGranularity = flex_ratio;
		}

		busFreq = EFI_get_frequency("FSBFrequency");
		/* If EFI isn't configured correctly, use a constant 
		 * value. See 6036811.
		 */
		if (busFreq == 0)
		    busFreq = BASE_NHM_CLOCK_SOURCE;

		break;
            }
	case CPUFAMILY_INTEL_PENRYN: {
		uint64_t	prfsts;

		prfsts = rdmsr64(IA32_PERF_STS);
		tscGranularity = (uint32_t)bitfield(prfsts, 44, 40);
		N_by_2_bus_ratio = (prfsts & bit(46)) != 0;

		busFreq = EFI_get_frequency("FSBFrequency");
	    }
	}
    }
    else{
	switch (cpuid_info()->cpuid_family){
        case 6:  /*** AMD Family 06h ***/
		case 21: /*** AMD Family 15h Bulldozer ***/
		{
            uint64_t cofvid = 0;
            uint64_t cpuFreq = 0;
            uint64_t cpuMult;
            uint64_t divisor;
            uint64_t did;
            uint64_t fid;
            
            cofvid  = rdmsr64(AMD_COFVID_STATUS);
            did = bitfield(cofvid, 8, 6);
            fid = bitfield(cofvid, 5, 0);
            
            if (did == 0) divisor = 2;
            else if (did == 1) divisor = 4;
            else if (did == 2) divisor = 8;
            else if (did == 3) divisor = 16;
            else if (did == 4) divisor = 32;
            
            cpuMult = ((fid + 16) * 10) / divisor;
            cpuFreq = EFI_CPU_Frequency();
            busFreq = (cpuFreq * 10) / cpuMult;
            tscGranularity = cpuMult / 10;
            break;
		}
        case 22: /*** AMD Family 16h Jaguar ***/
        {
            uint64_t cofvid = 0;
            uint64_t cpuFreq = 0;
            uint64_t cpuMult;
            uint64_t divisor;
            uint64_t did;
            uint64_t fid;
            
            cofvid  = rdmsr64(AMD_COFVID_STATUS);
            did = bitfield(cofvid, 8, 6);
            fid = bitfield(cofvid, 5, 0);
            if (did == 0) divisor = 1;
            else if (did == 1) divisor = 2;
            else if (did == 2) divisor = 4;
            else if (did == 3) divisor = 8;
            else if (did == 4) divisor = 16;
            
            cpuMult = ((fid + 16) * 10) / divisor;
            cpuFreq = EFI_CPU_Frequency();
            
            if (cofvid & (uint64_t)bit(0)) {
                busFreq = (cpuFreq * 2)/((cpuMult*2)+1);
            }
            else{
                busFreq = cpuFreq / cpuMult;
            }
            tscGranularity = cpuMult;
            break;
        }
		case 23: /*** AMD Family 17h Zen ***/
        {
            uint64_t CpuDfsId;
            uint64_t CpuFid;
            uint64_t cpuMult;
            int64_t cofvid = 0;
            uint64_t cpuFreq = 0;
            uint64_t divisor;
            
            cofvid = rdmsr64(AMD_PSTATE0_STS);
            
            CpuDfsId = bitfield(cofvid, 13, 8);
            CpuFid = bitfield(cofvid, 7, 0);
            cpuMult = (CpuFid * 10 / CpuDfsId) * 2;
            busFreq = EFI_get_frequency("FSBFrequency");
            tscFreq = busFreq * cpuMult / 10;
            tscGranularity = cpuMult;
            
            if (busFreq == 0) {
                busFreq = 1000000000ULL;
                
            }
            
            break;
        }
    }
    }

	if (busFreq != 0) {
		busFCvtt2n = ((1 * Giga) << 32) / busFreq;
		busFCvtn2t = 0xFFFFFFFFFFFFFFFFULL / busFCvtt2n;
	} else {
		busFreq = 200*Mega;
		kprintf("rtclock_init: Setting fsb to %u MHz\n", (uint32_t) (busFreq/Mega));
	}

    kprintf(" BUS: Frequency = %6d.%06dMHz, "
		"cvtt2n = %08X.%08X, cvtn2t = %08X.%08X\n",
		(uint32_t)(busFreq / Mega),
		(uint32_t)(busFreq % Mega), 
		(uint32_t)(busFCvtt2n >> 32), (uint32_t)busFCvtt2n,
		(uint32_t)(busFCvtn2t >> 32), (uint32_t)busFCvtn2t);

	if (tscFreq == busFreq) {
		bus2tsc = 1;
		tscGranularity = 1;
		tscFCvtn2t = busFCvtn2t;
		tscFCvtt2n = busFCvtt2n;
	} else {
		/*
		 * Get the TSC increment.  The TSC is incremented by this
		 * on every bus tick.  Calculate the TSC conversion factors
		 * to and from nano-seconds.
		 * The tsc granularity is also called the "bus ratio".
		 * If the N/2 bit is set this indicates the bus ration is
		 * 0.5 more than this - i.e.  that the true bus ratio
		 * is (2*tscGranularity + 1)/2.
		 */
		if (N_by_2_bus_ratio)
			tscFCvtt2n = busFCvtt2n * 2 / (1 + 2*tscGranularity);
		else
			tscFCvtt2n = busFCvtt2n / tscGranularity;

        tscFreq = EFI_CPU_Frequency();
		tscFCvtn2t = 0xFFFFFFFFFFFFFFFFULL / tscFCvtt2n;

		/*
		 * Calculate conversion from BUS to TSC
		 */
		bus2tsc = tmrCvt(busFCvtt2n, tscFCvtn2t);
	}

	kprintf(" TSC: Frequency = %6d.%06dMHz, "
		"cvtt2n = %08X.%08X, cvtn2t = %08X.%08X, gran = %lld%s\n",
		(uint32_t)(tscFreq / Mega),
		(uint32_t)(tscFreq % Mega), 
		(uint32_t)(tscFCvtt2n >> 32), (uint32_t)tscFCvtt2n,
		(uint32_t)(tscFCvtn2t >> 32), (uint32_t)tscFCvtn2t,
		tscGranularity, N_by_2_bus_ratio ? " (N/2)" : "");
}

void
tsc_get_info(tscInfo_t *info)
{
	info->busFCvtt2n     = busFCvtt2n;
	info->busFCvtn2t     = busFCvtn2t;
	info->tscFreq        = tscFreq;
	info->tscFCvtt2n     = tscFCvtt2n;
	info->tscFCvtn2t     = tscFCvtn2t;
	info->tscGranularity = tscGranularity;
	info->bus2tsc        = bus2tsc;
	info->busFreq        = busFreq;
	info->flex_ratio     = flex_ratio;
	info->flex_ratio_min = flex_ratio_min;
	info->flex_ratio_max = flex_ratio_max;
}
