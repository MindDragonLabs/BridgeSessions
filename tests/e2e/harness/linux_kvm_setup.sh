#!/usr/bin/env bash
# Provision bs-qa-ubuntu desktop guest on linux-a for automated Linux CUA/tray.
# Run ON linux-a as user agent (libvirt group). Idempotent.
#
# Guest: Ubuntu 24.04 cloud image + cloud-init (user agent, passwordless sudo,
#        XFCE auto-login, openssh, xdotool, bridgesessions binary optional).
set -euo pipefail

VM_NAME="${BS_QA_VM_NAME:-bs-qa-ubuntu}"
IMG_DIR="${BS_QA_IMG_DIR:-/var/lib/libvirt/images}"
WORK="${HOME}/bs-qa-kvm"
BASE_CLOUD="${IMG_DIR}/ubuntu-cloud.qcow2"
DISK="${IMG_DIR}/${VM_NAME}.qcow2"
SEED="${IMG_DIR}/${VM_NAME}-seed.iso"
MEM_MB="${BS_QA_MEM_MB:-4096}"
VCPUS="${BS_QA_VCPUS:-4}"
DISK_GB="${BS_QA_DISK_GB:-40}"

echo "→ bs-qa KVM setup name=$VM_NAME"

if [[ ! -e /dev/kvm ]]; then
  echo "ERROR: /dev/kvm missing" >&2
  exit 1
fi

# Ensure default network
if ! virsh net-info default >/dev/null 2>&1; then
  echo "ERROR: libvirt network 'default' missing" >&2
  exit 1
fi
virsh net-start default 2>/dev/null || true
virsh net-autostart default 2>/dev/null || true

mkdir -p "$WORK"

# If VM already running, report and exit
if virsh domstate "$VM_NAME" 2>/dev/null | grep -qi running; then
  echo "→ $VM_NAME already running"
  virsh domifaddr "$VM_NAME" 2>/dev/null || true
  echo "KVM_READY"
  exit 0
fi

# If defined but shut off, start
if virsh dominfo "$VM_NAME" >/dev/null 2>&1; then
  echo "→ starting existing $VM_NAME"
  virsh start "$VM_NAME"
  sleep 5
  virsh domifaddr "$VM_NAME" 2>/dev/null || true
  echo "KVM_READY"
  exit 0
fi

if [[ ! -f "$BASE_CLOUD" ]]; then
  echo "ERROR: base cloud image missing: $BASE_CLOUD" >&2
  echo "Download: https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img" >&2
  exit 1
fi

if ! command -v cloud-localds >/dev/null 2>&1 && ! command -v genisoimage >/dev/null 2>&1; then
  echo "→ installing cloud-image-utils / cdrtools if possible"
  sudo pacman -S --noconfirm cloud-image-utils cdrtools 2>/dev/null || true
fi

# Clone disk
if [[ ! -f "$DISK" ]]; then
  echo "→ cloning cloud image → $DISK"
  sudo qemu-img create -f qcow2 -F qcow2 -b "$BASE_CLOUD" "$DISK" "${DISK_GB}G"
  sudo chown libvirt-qemu:kvm "$DISK" 2>/dev/null || sudo chown "$USER":"$USER" "$DISK"
fi

# cloud-init user-data
USER_DATA="$WORK/user-data"
META_DATA="$WORK/meta-data"
cat > "$META_DATA" <<EOF
instance-id: ${VM_NAME}
local-hostname: ${VM_NAME}
EOF

# Password: change in vault for production QA; local libvirt only
# Use SSH key from agent if available
PUBKEY=""
if [[ -f "$HOME/.ssh/id_ed25519.pub" ]]; then
  PUBKEY=$(cat "$HOME/.ssh/id_ed25519.pub")
elif [[ -f "$HOME/.ssh/authorized_keys" ]]; then
  PUBKEY=$(head -1 "$HOME/.ssh/authorized_keys")
fi

cat > "$USER_DATA" <<EOF
#cloud-config
users:
  - name: agent
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    groups: [sudo, adm]
    lock_passwd: false
    plain_text_passwd: 'bsqa-agent'
    ssh_authorized_keys:
      - ${PUBKEY:-ssh-ed25519 AAAA invalid-placeholder}
package_update: true
packages:
  - xfce4
  - xfce4-goodies
  - xfce4-terminal
  - lightdm
  - lightdm-gtk-greeter
  - xdotool
  - scrot
  - imagemagick
  - python3-pip
  - python3-gi
  - gir1.2-ayatanaappindicator3-0.1
  - libappindicator3-1
  - dbus-x11
  - openssh-server
write_files:
  - path: /etc/lightdm/lightdm.conf.d/50-bs-autologin.conf
    content: |
      [Seat:*]
      autologin-user=agent
      autologin-user-timeout=0
      user-session=xfce
  - path: /home/agent/.config/autostart/bs-display-ready.desktop
    permissions: '0644'
    content: |
      [Desktop Entry]
      Type=Application
      Name=BS Display Ready
      Exec=/bin/bash -c 'echo DISPLAY_READY > /tmp/bs-display-ready; touch /tmp/.bs-xauth'
      X-GNOME-Autostart-enabled=true
runcmd:
  - systemctl enable lightdm || true
  - systemctl set-default graphical.target
  - mkdir -p /home/agent/.config/autostart
  - chown -R agent:agent /home/agent
  - echo "export DISPLAY=:0" >> /home/agent/.bashrc
final_message: "bs-qa-ubuntu cloud-init done"
EOF

# Build seed ISO
rm -f "$SEED"
if command -v cloud-localds >/dev/null 2>&1; then
  cloud-localds "$SEED" "$USER_DATA" "$META_DATA"
elif command -v genisoimage >/dev/null 2>&1; then
  mkdir -p "$WORK/seed"
  cp "$USER_DATA" "$WORK/seed/user-data"
  cp "$META_DATA" "$WORK/seed/meta-data"
  genisoimage -output "$SEED" -volid cidata -joliet -rock "$WORK/seed/user-data" "$WORK/seed/meta-data"
else
  echo "ERROR: need cloud-localds or genisoimage" >&2
  exit 1
fi
sudo mv -f "$SEED" "${IMG_DIR}/${VM_NAME}-seed.iso" 2>/dev/null || true
SEED="${IMG_DIR}/${VM_NAME}-seed.iso"

echo "→ virt-install $VM_NAME"
virt-install \
  --name "$VM_NAME" \
  --memory "$MEM_MB" \
  --vcpus "$VCPUS" \
  --disk "path=$DISK,format=qcow2,bus=virtio" \
  --disk "path=$SEED,device=cdrom" \
  --os-variant ubuntu24.04 \
  --import \
  --network network=default,model=virtio \
  --graphics vnc,listen=127.0.0.1 \
  --noautoconsole \
  --wait 0

echo "→ waiting for guest boot (cloud-init may take 3–8 min)…"
for i in $(seq 1 60); do
  sleep 10
  ADDR=$(virsh domifaddr "$VM_NAME" 2>/dev/null | awk '/ipv4/ {print $4}' | cut -d/ -f1 | head -1 || true)
  if [[ -n "$ADDR" ]]; then
    echo "→ guest IP $ADDR"
    if ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no agent@"$ADDR" 'echo SSH_OK' 2>/dev/null; then
      echo "KVM_READY ip=$ADDR"
      exit 0
    fi
  fi
  echo "  … still waiting ($i)"
done

echo "WARN: VM started but SSH not ready yet — check: virsh console $VM_NAME" >&2
echo "KVM_STARTED_PENDING_SSH"
exit 0
