/* drivers/acpi/sleep/power.c - PM core functionality for Xen
 *
 * Copyrights from Linux side:
 * Copyright (c) 2000-2003 Patrick Mochel
 * Copyright (C) 2001-2003 Pavel Machek <pavel@suse.cz>
 * Copyright (c) 2003 Open Source Development Lab
 * Copyright (c) 2004 David Shaohua Li <shaohua.li@intel.com>
 * Copyright (c) 2005 Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>
 *
 * Slimmed with Xen specific support.
 */

#include <xen/config.h>
#include <asm/io.h>
#include <asm/acpi.h>
#include <xen/acpi.h>
#include <xen/errno.h>
#include <xen/iocap.h>
#include <xen/sched.h>
#include <asm/acpi.h>
#include <asm/irq.h>
#include <asm/init.h>
#include <xen/spinlock.h>
#include <xen/sched.h>
#include <xen/domain.h>
#include <xen/console.h>
#include <public/platform.h>

#define pmprintk(_l, _f, _a...) printk(_l "<PM>" _f, ## _a )

static char opt_acpi_sleep[20];
string_param("acpi_sleep", opt_acpi_sleep);

static u8 sleep_states[ACPI_S_STATE_COUNT];
static DEFINE_SPINLOCK(pm_lock);

struct acpi_sleep_info acpi_sinfo;

void do_suspend_lowlevel(void);

static char *acpi_states[ACPI_S_STATE_COUNT] =
{
    [ACPI_STATE_S1] = "standby",
    [ACPI_STATE_S3] = "mem",
    [ACPI_STATE_S4] = "disk",
};

static int device_power_down(void)
{
    console_suspend();

    time_suspend();

    i8259A_suspend();
    
    ioapic_suspend();
    
    lapic_suspend();

    return 0;
}

static void device_power_up(void)
{
    lapic_resume();
    
    ioapic_resume();

    i8259A_resume();
    
    time_resume();

    console_resume();
}

static void freeze_domains(void)
{
    struct domain *d;

    for_each_domain(d)
        if (d->domain_id != 0)
            domain_pause(d);
}

static void thaw_domains(void)
{
    struct domain *d;

    for_each_domain(d)
        if (d->domain_id != 0)
            domain_unpause(d);
}

static void acpi_sleep_prepare(u32 state)
{
    void *wakeup_vector_va;

    if ( state != ACPI_STATE_S3 )
        return;

    wakeup_vector_va = __acpi_map_table(
        acpi_sinfo.wakeup_vector, sizeof(uint64_t));
    if (acpi_sinfo.vector_width == 32)
        *(uint32_t *)wakeup_vector_va =
            (uint32_t)bootsym_phys(wakeup_start);
    else
        *(uint64_t *)wakeup_vector_va =
            (uint64_t)bootsym_phys(wakeup_start);
}

static void acpi_sleep_post(u32 state) {}

/* Main interface to do xen specific suspend/resume */
static int enter_state(u32 state)
{
    unsigned long flags;
    int error;

    if (state <= ACPI_STATE_S0 || state > ACPI_S_STATES_MAX)
        return -EINVAL;

    /* Sync lazy state on ths cpu */
    __sync_lazy_execstate();
    pmprintk(XENLOG_INFO, "Flush lazy state\n");

    if (!spin_trylock(&pm_lock))
        return -EBUSY;
    
    freeze_domains();

    hvm_cpu_down();

    pmprintk(XENLOG_INFO, "PM: Preparing system for %s sleep\n",
        acpi_states[state]);

    acpi_sleep_prepare(state);

    local_irq_save(flags);

    if ((error = device_power_down()))
    {
        printk(XENLOG_ERR "Some devices failed to power down\n");
        goto Done;
    }

    ACPI_FLUSH_CPU_CACHE();

    switch (state)
    {
        case ACPI_STATE_S3:
            do_suspend_lowlevel();
            break;
        default:
            error = -EINVAL;
            break;
    }

    pmprintk(XENLOG_INFO, "Back to C!\n");

    device_power_up();

    pmprintk(XENLOG_INFO, "PM: Finishing wakeup.\n");

 Done:
    local_irq_restore(flags);

    acpi_sleep_post(state);

    if ( !hvm_cpu_up() )
        BUG();

    thaw_domains();
    spin_unlock(&pm_lock);
    return error;
}

/*
 * Dom0 issues this hypercall in place of writing pm1a_cnt. Xen then
 * takes over the control and put the system into sleep state really.
 * Also video flags and mode are passed here, in case user may use
 * "acpi_sleep=***" for video resume.
 *
 * Guest may issue a two-phases write to PM1x_CNT, to work
 * around poorly implemented hardware. It's better to keep
 * this logic here. Two writes can be differentiated by 
 * enable bit setting.
 */
int acpi_enter_sleep(struct xenpf_enter_acpi_sleep *sleep)
{
    if ( !IS_PRIV(current->domain) || !acpi_sinfo.pm1a_cnt )
        return -EPERM;

    /* Sanity check */
    if ( acpi_sinfo.pm1b_cnt_val &&
         ((sleep->pm1a_cnt_val ^ sleep->pm1b_cnt_val) &
          ACPI_BITMASK_SLEEP_ENABLE) )
    {
        pmprintk(XENLOG_ERR, "Mismatched pm1a/pm1b setting\n");
        return -EINVAL;
    }

    if ( sleep->flags )
        return -EINVAL;

    /* Write #1 */
    if ( !(sleep->pm1a_cnt_val & ACPI_BITMASK_SLEEP_ENABLE) )
    {
        outw((u16)sleep->pm1a_cnt_val, acpi_sinfo.pm1a_cnt);
        if (acpi_sinfo.pm1b_cnt)
            outw((u16)sleep->pm1b_cnt_val, acpi_sinfo.pm1b_cnt);
        return 0;
    }

    /* Write #2 */
    acpi_sinfo.pm1a_cnt_val = sleep->pm1a_cnt_val;
    acpi_sinfo.pm1b_cnt_val = sleep->pm1b_cnt_val;
    acpi_sinfo.sleep_state = sleep->sleep_state;

    return enter_state(acpi_sinfo.sleep_state);
}

static int acpi_get_wake_status(void)
{
    uint16_t val;

    /* Wake status is the 15th bit of PM1 status register. (ACPI spec 3.0) */
    val = inw(acpi_sinfo.pm1a_evt) | inw(acpi_sinfo.pm1b_evt);
    val &= ACPI_BITMASK_WAKE_STATUS;
    val >>= ACPI_BITPOSITION_WAKE_STATUS;
    return val;
}

/* System is really put into sleep state by this stub */
acpi_status asmlinkage acpi_enter_sleep_state(u8 sleep_state)
{
    ACPI_FLUSH_CPU_CACHE();

    outw((u16)acpi_sinfo.pm1a_cnt_val, acpi_sinfo.pm1a_cnt);
    if (acpi_sinfo.pm1b_cnt)
        outw((u16)acpi_sinfo.pm1b_cnt_val, acpi_sinfo.pm1b_cnt);

    /* Wait until we enter sleep state, and spin until we wake */
    while (!acpi_get_wake_status());
    return_ACPI_STATUS(AE_OK);
}

static int __init acpi_sleep_init(void)
{
    int i;
    char *p = opt_acpi_sleep;

    while ( (p != NULL) && (*p != '\0') )
    {
        if ( !strncmp(p, "s3_bios", 7) )
            acpi_video_flags |= 1;
        if ( !strncmp(p, "s3_mode", 7) )
            acpi_video_flags |= 2;
        p = strchr(p, ',');
        if ( p != NULL )
            p += strspn(p, ", \t");
    }

    pmprintk(XENLOG_INFO, "ACPI (supports");
    for ( i = 0; i < ACPI_S_STATE_COUNT; i++ )
    {
        if ( i == ACPI_STATE_S3 )
        {
            sleep_states[i] = 1;
            printk(" S%d", i);
        }
        else
            sleep_states[i] = 0;
    }
    printk(")\n");

    return 0;
}
__initcall(acpi_sleep_init);
