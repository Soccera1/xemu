Please visit [https://xemu.app](https://xemu.app) for more information.

### Linux KVM acceleration

On x86 Linux hosts, KVM can be enabled in `xemu.toml` (restart required):

```toml
[sys]
accel = "kvm"
```

The default is `"tcg"`. The KVM setting tries KVM first and falls back to TCG
if KVM initialization fails. For an explicit KVM-only launch, use
`xemu -accel kvm`. Command-line accelerator options override the setting.
The user running xemu needs read/write access to `/dev/kvm`, and the build
must include KVM support (`--enable-kvm` when configuring explicitly).

> **Warning:** With KVM, the guest CPU runs at the host's speed. Games that rely
> on the original Xbox CPU's speed can run much too fast on modern hardware.
> If this causes problems, disable KVM by setting `accel = "tcg"` under `[sys]`
> and restarting xemu, or launch with `xemu -accel tcg`.

On hosts with KVM TSC scaling support, the guest timestamp-counter frequency
can be set in kilohertz (restart required):

```toml
[sys]
accel = "kvm"
kvm_tsc_khz = 733333
```

`733333` approximates the original Xbox's 733.333333 MHz TSC rate. The default,
`0`, keeps the host's TSC rate. This option is ignored with TCG.
KVM initialization checks host scaling support and whether the kernel can apply
the requested rate. If either check fails, the configured KVM accelerator falls
back to TCG; an explicit `-accel kvm` launch reports an error instead.

TSC scaling changes the timestamp counter's frequency. CPU instructions still
execute at host speed, so games that depend on busy-loop timing may still run
too fast.

Some custom BIOSes include CPU-upgrade compatibility patches that may help
with timing problems in some games. For example,
[Cerbios 3.0.0 Beta](https://xbins.org/nfo.php?file=xboxnfo2527.nfo) introduced
`AdvCPUSupport` to patch executables for upgraded or overclocked CPUs.
These fixes have not been verified with xemu's KVM backend and may not resolve
every game's timing issues. Switch back to TCG if problems persist.

CPU-clock-sensitive examples include:

- **Grand Theft Auto: San Andreas** — the CPU patcher's author documents
  [hard-coded CPU-frequency values in its executable](https://www.reddit.com/r/originalxbox/comments/1dhrsp2/).
- **Crash Nitro Kart**, **MechAssault**, and **MechAssault 2** —
  [reported to run too fast on a CPU-upgraded 1.4 GHz Xbox](https://www.reddit.com/r/originalxbox/comments/1ldqxue/14ghz_128mb_xbox_patching_games_to_runbios/).

This is not an exhaustive list. The evidence above concerns executable
patching and modified physical consoles; the exact behavior of these titles
under xemu's KVM backend has not yet been verified.

The host must support the Pentium III CPU features and execute real mode and
legacy 32-bit protected mode guests. xemu tests these guest modes in a temporary
VM before starting the Xbox. IA-32e (64-bit guest) support alone is insufficient
on hosts without unrestricted guest or equivalent legacy guest support.
The checks include legacy 32-bit paging and SSE execution.
The CPU model remains Pentium III; do not select `-cpu host` as a workaround.

To validate a host, compare booting and running the same software with
`-accel kvm` and `-accel tcg`, including rebooting and restoring snapshots.
For hardware parity testing, use an XBE that checks CPUID and SSE results,
exercises PIC/PIT timer interrupts, and reads back a GPU render target through
the CPU. Compare RDTSC deltas against PIT elapsed time to check the TSC rate.
Run the same XBE on an original Xbox and with both accelerators.
