#!/bin/bash
# RK3588 热点配置脚本

set -euo pipefail

# ====================  可配置参数 ====================
SSID="RK3588"                             # 最长 32 字节
PASSPHRASE="qidianrk3588"                 # 8-63 字节
WLAN_MAC="${WLAN_MAC:-f8:54:f6:c0:6b:6e}" # 目标网卡的永久MAC
HW_MODE="a"                    # a=5GHz/信道36；g=2.4GHz/信道6
HOTSPOT_IP="192.168.4.1"       # 手机访问目标设备时使用的地址

# ==================== 2. 必要的运行环境检查 ====================
# 配置系统文件需要 root 权限；检查依赖及网卡，避免配置到一半才发现环境不完整。
if (( $(id -u) != 0 )); then
    echo "请在目标设备宿主机执行：sudo bash hotspot_setup.sh" >&2
    exit 1
fi
for tool in ip systemctl iw hostapd dhcpd; do
    command -v "$tool" >/dev/null || {
        echo "缺少命令 $tool，请先按 README 安装依赖。" >&2
        exit 1
    }
done
# 按 MAC 查找当前接口名；同时支持 ip 输出中的当前地址和永久地址 permaddr。
WLAN_MAC=${WLAN_MAC,,}
WLAN_IFACE=$(ip -o link show | awk -v mac="$WLAN_MAC" '
    {for (i=1; i<NF; i++) {
        if (($i == "link/ether" || $i == "permaddr") && tolower($(i+1)) == mac) {
            name=$2; sub(/:$/, "", name); sub(/@.*/, "", name); print name; break
        }
    }}')
if [[ -z "$WLAN_IFACE" || "$WLAN_IFACE" == *$'\n'* ]] ||
   ! iw dev "$WLAN_IFACE" info >/dev/null 2>&1; then
    echo "无法唯一识别无线网卡（MAC=$WLAN_MAC），请用 ip -o link show 和 iw dev 核对。" >&2
    exit 1
fi

# ==================== 3. 根据配置参数选择信道与DHCP地址池 ====================
case "$HW_MODE" in
    a) CHANNEL=36 ;;
    g) CHANNEL=6 ;;
    *) echo "HW_MODE 只能为 a 或 g" >&2; exit 1 ;;
esac
SUBNET="${HOTSPOT_IP%.*}"
DHCP_START=100
DHCP_END=200
case "${HOTSPOT_IP##*.}" in
    1[0-9][0-9]|200) DHCP_START=2; DHCP_END=99 ;;
esac

# 防止热点与有线等其他接口使用同一网段，导致路由冲突。
if ip -4 -o addr show scope global | awk -v iface="$WLAN_IFACE" -v prefix="$SUBNET." '
    $2 != iface && index($4, prefix) == 1 {found=1} END {exit !found}'; then
    echo "热点网段 $SUBNET.0/24 已被其他网卡占用，请修改 HOTSPOT_IP。" >&2
    exit 1
fi

# ==================== 4. 准备配置目录与文件权限 ====================
# 新建配置文件默认仅 root 可读写，保护热点密码。
umask 077
mkdir -p /etc/hostapd /etc/default /etc/dhcp /etc/NetworkManager/conf.d /usr/local/sbin

# ==================== 5. 配置 hostapd 热点广播 ====================
# 写入无线接口、热点名称、频段和 WPA2 密码，并指定 hostapd 配置路径。
cat > /etc/hostapd/hostapd.conf <<EOF
# Managed by rk3588-hotspot
interface=$WLAN_IFACE
driver=nl80211
ssid=$SSID
hw_mode=$HW_MODE
channel=$CHANNEL
wmm_enabled=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=$PASSPHRASE
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
EOF
touch /etc/default/hostapd
sed -i '/^DAEMON_CONF=/d' /etc/default/hostapd
printf 'DAEMON_CONF="/etc/hostapd/hostapd.conf"\n' >> /etc/default/hostapd

# ==================== 6. 配置 ISC DHCP 地址分配 ====================
# 仅在热点接口分配局域网地址；保留 defaults 文件中的其他选项。
touch /etc/default/isc-dhcp-server
sed -i '/^INTERFACESv[46]=/d' /etc/default/isc-dhcp-server
printf 'INTERFACESv4="%s"\nINTERFACESv6=""\n' "$WLAN_IFACE" >> /etc/default/isc-dhcp-server
cat > /etc/dhcp/dhcpd.conf <<EOF
# Managed by rk3588-hotspot
authoritative;
ddns-update-style none;
default-lease-time 600;
max-lease-time 7200;
subnet $SUBNET.0 netmask 255.255.255.0 {
    range $SUBNET.$DHCP_START $SUBNET.$DHCP_END;
    option subnet-mask 255.255.255.0;
}
EOF
# 写入完成后检查 DHCP 配置语法，确认服务可以读取。
dhcpd -t -cf /etc/dhcp/dhcpd.conf

# ==================== 7. 交由 hostapd 管理热点网卡 ====================
# 当前若正在连接其他 Wi-Fi，先断开，随后由 hostapd 接管。
if command -v nmcli >/dev/null; then
    nmcli device disconnect "$WLAN_IFACE" || true
fi
cat > /etc/NetworkManager/conf.d/90-rk3588-hotspot.conf <<EOF
[keyfile]
unmanaged-devices+=mac:$WLAN_MAC
EOF
chmod 644 /etc/NetworkManager/conf.d/90-rk3588-hotspot.conf
if command -v nmcli >/dev/null; then
    nmcli general reload conf || true
    nmcli device set "$WLAN_IFACE" managed no || true
fi

# ==================== 8. 生成开机网卡初始化脚本 ====================
# systemd 每次开机调用：等待网卡就绪、启用网卡并设置热点 IP。
{
    echo '#!/bin/bash'
    echo 'set -euo pipefail'
    printf 'WLAN_MAC=%q\nHOTSPOT_IP=%q\n' "$WLAN_MAC" "$HOTSPOT_IP"
    printf 'IP_BIN=%q\n' "$(command -v ip)"
    cat <<'HELPER'
# 根据 MAC 查找本次启动的实际接口名，优先匹配永久硬件地址。
find_interface() {
    "$IP_BIN" -o link show | awk -v mac="$WLAN_MAC" '
        {for (i=1; i<NF; i++) {
            if (($i == "link/ether" || $i == "permaddr") && tolower($(i+1)) == mac) {
                name=$2; sub(/:$/, "", name); sub(/@.*/, "", name); print name; break
            }
        }}'
}

# 服务停止时撤销这块网卡上的本热点地址。
if [[ ${1:-start} == stop ]]; then
    WLAN_IFACE=$(find_interface)
    if [[ -n "$WLAN_IFACE" && "$WLAN_IFACE" != *$'\n'* ]]; then
        "$IP_BIN" -4 addr del "$HOTSPOT_IP/24" dev "$WLAN_IFACE" 2>/dev/null || true
    fi
    exit 0
fi

# 开机时最多等待 60 秒，允许无线驱动稍后完成初始化。
ready=false
for attempt in {1..60}; do
    WLAN_IFACE=$(find_interface)
    if [[ -n "$WLAN_IFACE" && "$WLAN_IFACE" != *$'\n'* ]] &&
       "$IP_BIN" link set dev "$WLAN_IFACE" up; then ready=true; break; fi
    sleep 1
done
[[ "$ready" == true ]] || { echo "等待热点网卡失败（MAC=$WLAN_MAC），请检查驱动及 rfkill。" >&2; exit 1; }

# 同步广播和 DHCP 使用的接口名，再配置本地地址。
sed -i "s/^interface=.*/interface=$WLAN_IFACE/" /etc/hostapd/hostapd.conf
sed -i "s/^INTERFACESv4=.*/INTERFACESv4=\"$WLAN_IFACE\"/" /etc/default/isc-dhcp-server
"$IP_BIN" -4 addr replace "$HOTSPOT_IP/24" dev "$WLAN_IFACE"
echo "热点网卡就绪：接口=$WLAN_IFACE，MAC=$WLAN_MAC，IP=$HOTSPOT_IP"
HELPER
} > /usr/local/sbin/rk3588-hotspot-network
chmod 755 /usr/local/sbin/rk3588-hotspot-network

# ==================== 9. 注册网卡初始化服务 ====================
# 初始化服务先于热点广播和 DHCP 服务运行，开机自动恢复热点地址。
cat > /etc/systemd/system/rk3588-hotspot-network.service <<EOF
[Unit]
Description=Initialize RK3588 local hotspot address
After=NetworkManager.service systemd-udevd.service
Before=hostapd.service isc-dhcp-server.service
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/rk3588-hotspot-network
ExecStop=/usr/local/sbin/rk3588-hotspot-network stop
TimeoutStartSec=75
[Install]
WantedBy=multi-user.target
EOF
chmod 644 /etc/systemd/system/rk3588-hotspot-network.service

# ==================== 10. 设置热点服务的启动依赖 ====================
# 只有网卡和 IP 初始化成功后才启动 hostapd、DHCP；服务异常退出时自动重试。
for service in hostapd isc-dhcp-server; do
    mkdir -p "/etc/systemd/system/$service.service.d"
    cat > "/etc/systemd/system/$service.service.d/30-rk3588-hotspot.conf" <<EOF
[Unit]
Requires=rk3588-hotspot-network.service
After=rk3588-hotspot-network.service
[Service]
Restart=on-failure
RestartSec=3
EOF
    chmod 644 "/etc/systemd/system/$service.service.d/30-rk3588-hotspot.conf"
done

# ==================== 11. 应用配置并启用开机启动 ====================
# 重新加载 systemd 配置，按网卡初始化、热点广播/DHCP 的顺序启动服务。
systemctl daemon-reload
systemctl unmask hostapd.service isc-dhcp-server.service
systemctl enable rk3588-hotspot-network.service hostapd.service isc-dhcp-server.service
systemctl restart rk3588-hotspot-network.service
systemctl restart hostapd.service isc-dhcp-server.service

# ==================== 12. 显示服务状态与访问地址 ====================
# 检查服务是否正常运行，并提示在设备重启后再次验证热点。
systemctl --no-pager --full status rk3588-hotspot-network.service hostapd.service isc-dhcp-server.service
echo "热点已配置：SSID=$SSID，接口=$WLAN_IFACE，地址=$HOTSPOT_IP，DHCP=$SUBNET.$DHCP_START-$DHCP_END"
echo "已绑定网卡 MAC：$WLAN_MAC（开机自动识别实际接口名）"
echo "本地网页地址：http://$HOTSPOT_IP:8080"
echo "配置完成；请重启目标设备验证热点自启动。"