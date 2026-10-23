#!/usr/bin/env bash
# rig-info.sh: collects read-only facts about the audio rig into one text file.
# It changes nothing, needs no sudo, and sends nothing anywhere.
# Usage:  bash rig-info.sh            (writes ~/rig-info.txt)
#         bash rig-info.sh out.txt

out="${1:-$HOME/rig-info.txt}"

section() { printf '\n=== %s ===\n' "$1"; }
run() { "$@" 2>&1 || true; }
have() { command -v "$1" >/dev/null 2>&1; }

{
  section "date";        run date -Is
  section "uname";       run uname -a
  section "os-release";  run cat /etc/os-release
  section "kernel cmdline"; run cat /proc/cmdline
  section "kernel preempt config"
  run grep -E 'CONFIG_PREEMPT|CONFIG_HZ=|CONFIG_NO_HZ|CONFIG_IRQ_FORCED_THREADING' "/boot/config-$(uname -r)"

  section "cpu";         run lscpu
  section "P-cores (cpu_core)"; run cat /sys/devices/cpu_core/cpus
  section "E-cores (cpu_atom)"; run cat /sys/devices/cpu_atom/cpus
  section "SMT control"; run cat /sys/devices/system/cpu/smt/control
  section "intel_pstate"
  for f in status no_turbo min_perf_pct max_perf_pct; do
    printf '%s: ' "$f"; run cat "/sys/devices/system/cpu/intel_pstate/$f"
  done
  section "governors (count per value)"
  run sh -c 'cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort | uniq -c'
  section "energy_performance_preference (count per value)"
  run sh -c 'cat /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference | sort | uniq -c'
  section "max freq per cpu (kHz)"
  run sh -c 'for c in /sys/devices/system/cpu/cpu[0-9]*; do printf "%s %s\n" "${c##*/}" "$(cat $c/cpufreq/cpuinfo_max_freq 2>/dev/null)"; done'
  section "intel_idle max_cstate"; run cat /sys/module/intel_idle/parameters/max_cstate
  section "sensors"; have sensors && run sensors

  section "memory";      run free -h
  section "swappiness";  run cat /proc/sys/vm/swappiness

  section "realtime limits for this user"
  printf 'ulimit -r (rtprio): '; ulimit -r
  printf 'ulimit -l (memlock): '; ulimit -l
  printf 'groups: '; id -nG
  run sh -c 'grep -hv "^#" /etc/security/limits.conf /etc/security/limits.d/*.conf 2>/dev/null | grep -E "rtprio|memlock|nice" '
  section "rtirq"; have systemctl && run systemctl status rtirq --no-pager -n 0
  section "irqbalance"; run pgrep -a irqbalance
  section "USB interrupts"; run grep -iE 'xhci|ehci|usb' /proc/interrupts
  section "IRQ thread priorities (usb/xhci/snd)"
  run sh -c 'ps -eLo pid,cls,rtprio,psr,comm | grep -iE "irq/.*(xhci|usb|snd)" '
  section "USB autosuspend"; run cat /sys/module/usbcore/parameters/autosuspend

  section "sound cards";    run cat /proc/asound/cards
  section "playback devices"; have aplay && run aplay -l
  section "capture devices";  have arecord && run arecord -l
  section "raw MIDI ports";   have amidi && run amidi -l
  section "ALSA sequencer ports"; have aconnect && run aconnect -l
  section "USB devices";      have lsusb && run lsusb
  section "sound servers running"
  run pgrep -a -f 'pipewire|wireplumber|pulseaudio|jackd|jackdbus|a2jmidid'

  section "wine";       have wine && run wine --version
  section "yabridge";   have yabridgectl && run yabridgectl status
  section "plug-in folders (names only, two levels)"
  for d in "$HOME/.vst3" "$HOME/.clap" "$HOME/.lv2" \
           /usr/lib/vst3 /usr/lib/clap /usr/lib/lv2 \
           /usr/local/lib/vst3 /usr/local/lib/clap /usr/local/lib/lv2 \
           "$HOME/.wine/drive_c/Program Files/Common Files/VST3" \
           "$HOME/.wine/drive_c/Program Files/Common Files/CLAP"; do
    if [ -d "$d" ]; then
      printf -- '--- %s\n' "$d"
      run find "$d" -maxdepth 2 -printf '%P\n'
    fi
  done

  section "display"
  printf 'session type: %s\n' "${XDG_SESSION_TYPE:-unknown}"
  have xrandr && run xrandr --query
  have xinput && run xinput list

  section "build tools"
  have gcc && run gcc --version
  have g++ && run g++ --version
  have cmake && run cmake --version
  run ldd --version
} > "$out"

echo "Wrote $out"
