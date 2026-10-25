/*
 * etd_schema.c - 选项 schema（唯一事实来源）
 *
 * 本表的**行顺序**就是 easytier-core 的 argv 发射顺序，与
 * luci-app-easytier/root/etc/init.d/easytier 里 start_et() 的
 * `etcmd=etcmd` 分支（第 472~653 行）逐行对应，顺序错一个位置
 * 就会让生成出来的命令行与旧实现不一致。
 *
 * 默认值（.def）取自 init.d 的 `uci -q get ... || echo X` 兜底写法，
 * 即"argv 层面的事实默认值"。CBI 表单里的 .default 与之不同的项
 * （listenermode/tcp_port/ws_port/wss_port/desvice_name 等）在字段
 * 注释里单独标注，由渲染层决定表单初始值。
 *
 * label 取自 luasrc/model/cbi/easytier.lua 与 view/easytier/easytier_web.htm
 * 的 i18n 源串（逐字），help 为同一处英文说明的要点摘录。
 */
#include "etd_cfg.h"

#include <string.h>

const et_field et_schema[] = {
	/* ================= easytier-core（etcmd 模式，顺序 = 发射顺序） ================= */
	{ .name = "network_name", .kind = ET_K_PASS, .def = NULL,
	  .label = "Network Name", .tab = "general",
	  .help = "The network name used to identify this VPN network (--network-name parameter)",
	  .core_flag = "--network-name", .core_emit = ET_E_VAL },

	{ .name = "network_secret", .kind = ET_K_PASS, .def = NULL,
	  .label = "Network Secret", .tab = "general",
	  .help = "Network secret used to verify whether this node belongs to the VPN network (--network-secret parameter)",
	  .core_flag = "--network-secret", .core_emit = ET_E_VAL },

	{ .name = "ipaddr", .kind = ET_K_STR, .def = NULL,
	  .label = "Interface IP Address", .tab = "general",
	  .help = "The IPv4 address of this VPN node, empty means forward-only, no TUN device (-i parameter)",
	  .core_flag = "-i", .core_emit = ET_E_VAL },

	{ .name = "ip6addr", .kind = ET_K_STR, .def = NULL,
	  .label = "Interface IPV6 Address", .tab = "general",
	  .help = "IPv6 address of this VPN node, can be combined with IPv4 (--ipv6 parameter)",
	  .core_flag = "--ipv6", .core_emit = ET_E_VAL },

	{ .name = "ip_dhcp", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable DHCP", .tab = "general",
	  .help = "IP address is assigned by EasyTier automatically (-d parameter)",
	  .core_flag = "-d", .core_emit = ET_E_YES },

	{ .name = "peeradd", .kind = ET_K_LIST, .def = NULL,
	  .label = "Peer Nodes", .tab = "general",
	  .help = "Initial connected peer nodes (-p parameter)",
	  .core_flag = "-p", .core_emit = ET_E_REPEAT },

	{ .name = "proxy_network", .kind = ET_K_LIST, .def = NULL,
	  .label = "Subnet Proxy", .tab = "general",
	  .help = "Export the local network to other peers in the VPN (-n parameter)",
	  .core_flag = "-n", .core_emit = ET_E_REPEAT },

	{ .name = "exit_nodes", .kind = ET_K_LIST, .def = NULL,
	  .label = "Exit Node Addresses", .tab = "privacy",
	  .help = "Exit nodes to forward all traffic through, order defines priority (--exit-nodes parameter)",
	  .core_flag = "--exit-nodes", .core_emit = ET_E_REPEAT },

	{ .name = "manual_routes", .kind = ET_K_LIST, .def = NULL,
	  .label = "Route CIDR", .tab = "privacy",
	  .help = "Manually assign route CIDRs, disables subnet proxying and WireGuard routes (--manual-routes parameter)",
	  .core_flag = "--manual-routes", .core_emit = ET_E_REPEAT },

	/* relay_network 与 whitelist 是一组：只有 relay_network=1 时才发射
	 * --relay-network-whitelist；whitelist 为空时输出**裸旗标**（旧实现的怪行为，保留） */
	{ .name = "relay_network", .kind = ET_K_BOOL, .def = "0",
	  .label = "Forward Whitelisted Network Traffic", .tab = "privacy",
	  .help = "Only forward traffic for whitelisted networks. By default all networks are allowed",
	  .core_flag = "--relay-network-whitelist", .core_emit = ET_E_CUSTOM },

	{ .name = "whitelist", .kind = ET_K_LIST, .def = NULL,
	  .label = "Whitelisted Networks", .tab = "privacy",
	  .help = "Wildcard strings such as '*' or 'def*', empty disables forwarding (--relay-network-whitelist parameter)",
	  .core_flag = "--relay-network-whitelist", .core_emit = ET_E_NONE },

	{ .name = "mapped_listeners", .kind = ET_K_LIST, .def = NULL,
	  .label = "Public Addresses of Specified Listeners", .tab = "privacy",
	  .help = "Manually specify the public address of this machine (--mapped-listeners parameter)",
	  .core_flag = "--mapped-listeners", .core_emit = ET_E_REPEAT },

	{ .name = "port_forward", .kind = ET_K_LIST, .def = NULL,
	  .label = "Port Forwarding", .tab = "privacy",
	  .help = "Forward a local port to a remote port within the virtual network (--port-forward parameter)",
	  .core_flag = "--port-forward", .core_emit = ET_E_REPEAT },

	{ .name = "rpc_portal", .kind = ET_K_INT, .def = "15888",
	  .label = "Portal Address Port", .tab = "privacy",
	  .help = "RPC portal address, 0 means a random port (-r parameter)",
	  .core_flag = "-r", .core_emit = ET_E_VAL },

	{ .name = "rpc_portal_whitelist", .kind = ET_K_STR, .def = NULL,
	  .label = "RPC Access Whitelist", .tab = "privacy",
	  .help = "Only allow these addresses to access the rpc portal (--rpc-portal-whitelist parameter)",
	  .core_flag = "--rpc-portal-whitelist", .core_emit = ET_E_VAL },

	/* 监听端口：<scheme>_port -> -l <scheme>:<port>；tcp 额外再输出一条 udp（旧实现行为）。
	 * def 为 NULL：init.d 对这些项没有 || echo 兜底，未配置时**不输出任何 -l**，
	 * 让 easytier-core 用自身默认端口；CBI 表单默认值 11010/11011/11012 仅影响表单初值。 */
	{ .name = "tcp_port", .kind = ET_K_INT, .def = NULL,
	  .label = "TCP/UDP Port", .tab = "general",
	  .help = "TCP/UDP listen port, e.g. 11010 (writes both -l tcp: and -l udp:)",
	  .core_flag = "-l", .core_emit = ET_E_CUSTOM },

	{ .name = "ws_port", .kind = ET_K_INT, .def = NULL,
	  .label = "WS Port", .tab = "general",
	  .help = "WS listen port, e.g. 11011 (-l ws: parameter)",
	  .core_flag = "-l", .core_emit = ET_E_CUSTOM },

	{ .name = "wss_port", .kind = ET_K_INT, .def = NULL,
	  .label = "WSS Port", .tab = "general",
	  .help = "WSS listen port, e.g. 11012 (-l wss: parameter)",
	  .core_flag = "-l", .core_emit = ET_E_CUSTOM },

	{ .name = "wg_port", .kind = ET_K_INT, .def = NULL,
	  .label = "WG Port", .tab = "general",
	  .help = "WireGuard listen port, e.g. 11011 (-l wg: parameter)",
	  .core_flag = "-l", .core_emit = ET_E_CUSTOM },

	{ .name = "quic_port", .kind = ET_K_INT, .def = NULL,
	  .label = "QUIC Port", .tab = "general",
	  .help = "QUIC listen port (-l quic: parameter)",
	  .core_flag = "-l", .core_emit = ET_E_CUSTOM },

	{ .name = "external_node", .kind = ET_K_STR, .def = NULL,
	  .label = "Shared Node Address", .tab = "general",
	  .help = "Use a public shared node to discover peer nodes (-e parameter)",
	  .core_flag = "-e", .core_emit = ET_E_VAL },

	{ .name = "listenermode", .kind = ET_K_ENUM, .def = NULL,
	  .label = "Listener Port", .tab = "general",
	  .help = "OFF: do not listen on any port, only connect to peer nodes (--no-listener parameter)",
	  .enum_vals = "ON,OFF",
	  .core_flag = "--no-listener", .core_emit = ET_E_CUSTOM },

	{ .name = "desvice_name", .kind = ET_K_STR, .def = NULL,
	  .label = "Hostname", .tab = "general",
	  .help = "The hostname used to identify this device (--hostname parameter)",
	  .core_flag = "--hostname", .core_emit = ET_E_VAL },

	{ .name = "instance_name", .kind = ET_K_STR, .def = NULL,
	  .label = "Instance Name", .tab = "privacy",
	  .help = "Used to identify the VPN node instance on the same machine (-m parameter)",
	  .core_flag = "-m", .core_emit = ET_E_VAL },

	{ .name = "vpn_portal", .kind = ET_K_STR, .def = NULL,
	  .label = "VPN Portal Listen Address", .tab = "privacy",
	  .help = "WireGuard VPN portal listen address. Since EasyTier 2.7.0 only wg://HOST:PORT is accepted, "
		  "the old wg://HOST:PORT/CIDR form is skipped (--vpn-portal parameter)",
	  .core_flag = "--vpn-portal", .core_emit = ET_E_CUSTOM },

	{ .name = "vpn_portal_clients", .kind = ET_K_LIST, .def = NULL,
	  .label = "VPN Portal Clients", .tab = "privacy",
	  .help = "One WireGuard client per entry in the form NAME=CIDR (--vpn-portal-client parameter)",
	  .core_flag = "--vpn-portal-client", .core_emit = ET_E_REPEAT },

	/* 私钥不通过 argv 传递：start_et() 用 ET_VPN_PORTAL_PRIVATE_KEY 环境变量下发，
	 * 避免出现在进程命令行（状态页会展示命令行）。此处仅供表单与 et_argv_visible 脱敏。 */
	{ .name = "vpn_portal_private_key", .kind = ET_K_PASS, .def = NULL,
	  .label = "VPN Portal Private Key", .tab = "privacy",
	  .help = "Base64 encoded 32-byte WireGuard private key of the portal. Required since EasyTier 2.7.0, "
		  "handed to easytier-core through the ET_VPN_PORTAL_PRIVATE_KEY environment variable",
	  .core_flag = "--vpn-portal-private-key", .core_emit = ET_E_NONE },

	{ .name = "mtu", .kind = ET_K_INT, .def = NULL,
	  .label = "MTU", .tab = "privacy",
	  .help = "MTU for the TUN device, default is 1380 when unencrypted and 1360 when encrypted",
	  .core_flag = "--mtu", .core_emit = ET_E_VAL },

	{ .name = "default_protocol", .kind = ET_K_ENUM, .def = NULL,
	  .label = "Default Protocol", .tab = "privacy",
	  .help = "The default protocol used when connecting to peer nodes (--default-protocol parameter), "
		  "'-' or empty means use the core default",
	  .enum_vals = "-,tcp,udp,ws,wss",
	  .core_flag = "--default-protocol", .core_emit = ET_E_CUSTOM },

	{ .name = "tunname", .kind = ET_K_STR, .def = NULL,
	  .label = "Virtual Network Interface Name", .tab = "privacy",
	  .help = "Custom name for the virtual TUN interface (--dev-name parameter)",
	  .core_flag = "--dev-name", .core_emit = ET_E_VAL },

	{ .name = "disable_encryption", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable Encryption", .tab = "privacy",
	  .help = "Disable encryption for peer communication, all nodes must disable it together (-u parameter)",
	  .core_flag = "-u", .core_emit = ET_E_YES },

	/* 2.7.0 起 multi_thread 默认值由 false 改为 true，旧实现始终显式传值，
	 * 这里照抄：任何非 "0" 值都输出 true */
	{ .name = "multi_thread", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable Multithreading", .tab = "privacy",
	  .help = "Enable multithreaded operation; always passed explicitly as --multi-thread true|false",
	  .core_flag = "--multi-thread", .core_emit = ET_E_CUSTOM },

	{ .name = "multi_thread_count", .kind = ET_K_INT, .def = NULL,
	  .label = "Number of Threads", .tab = "privacy",
	  .help = "Number of threads, only effective when multi-thread is enabled (--multi-thread-count parameter)",
	  .core_flag = "--multi-thread-count", .core_emit = ET_E_VAL },

	{ .name = "no_tun", .kind = ET_K_BOOL, .def = "0",
	  .label = "No TUN Mode", .tab = "privacy",
	  .help = "Do not create a TUN device (--no-tun parameter)",
	  .core_flag = "--no-tun", .core_emit = ET_E_YES },

	{ .name = "smoltcp", .kind = ET_K_BOOL, .def = "0",
	  .label = "Use Userspace TCP/IP Stack", .tab = "privacy",
	  .help = "Enable smoltcp stack for subnet proxying (--use-smoltcp parameter)",
	  .core_flag = "--use-smoltcp", .core_emit = ET_E_YES },

	{ .name = "disable_ipv6", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable IPv6", .tab = "privacy",
	  .help = "Do not use IPv6 (--disable-ipv6 parameter)",
	  .core_flag = "--disable-ipv6", .core_emit = ET_E_YES },

	{ .name = "latency_first", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable Latency First", .tab = "privacy",
	  .help = "Attempt to forward traffic via the lowest latency path (--latency-first parameter)",
	  .core_flag = "--latency-first", .core_emit = ET_E_YES },

	{ .name = "exit_node", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable Exit Node", .tab = "privacy",
	  .help = "Allow this node to act as an exit node (--enable-exit-node parameter)",
	  .core_flag = "--enable-exit-node", .core_emit = ET_E_YES },

	{ .name = "disable_p2p", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable P2P", .tab = "privacy",
	  .help = "Disable P2P communication, only use nodes specified by -p (--disable-p2p parameter)",
	  .core_flag = "--disable-p2p", .core_emit = ET_E_YES },

	{ .name = "relay_all", .kind = ET_K_BOOL, .def = "0",
	  .label = "Allow Forwarding", .tab = "privacy",
	  .help = "Forward RPC packets from all peer nodes (--relay-all-peer-rpc parameter)",
	  .core_flag = "--relay-all-peer-rpc", .core_emit = ET_E_YES },

	{ .name = "socks_port", .kind = ET_K_INT, .def = NULL,
	  .label = "SOCKS5 Port", .tab = "privacy",
	  .help = "Enable a SOCKS5 server, leave blank to disable (--socks5 parameter)",
	  .core_flag = "--socks5", .core_emit = ET_E_VAL },

	{ .name = "comp", .kind = ET_K_ENUM, .def = "none",
	  .label = "Compression Algorithm", .tab = "privacy",
	  .help = "Compression algorithm to use, 'none' means no --compression parameter (--compression parameter)",
	  .enum_vals = "none,zstd",
	  .core_flag = "--compression", .core_emit = ET_E_CUSTOM },

	{ .name = "bind_device", .kind = ET_K_BOOL, .def = "0",
	  .label = "Bind to Physical NIC Only", .tab = "privacy",
	  .help = "Bind the connector socket to the physical device (--bind-device true|false parameter)",
	  .core_flag = "--bind-device", .core_emit = ET_E_CUSTOM },

	{ .name = "kcp_proxy", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable KCP Proxy", .tab = "privacy",
	  .help = "Convert TCP traffic to KCP traffic (--enable-kcp-proxy parameter)",
	  .core_flag = "--enable-kcp-proxy", .core_emit = ET_E_YES },

	{ .name = "kcp_input", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable KCP Input", .tab = "privacy",
	  .help = "Disallow other nodes from using KCP proxy TCP streams to this node (--disable-kcp-input parameter)",
	  .core_flag = "--disable-kcp-input", .core_emit = ET_E_YES },

	{ .name = "proxy_forward", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable Built-in NAT", .tab = "privacy",
	  .help = "Use the system kernel to forward subnet proxy packets (--proxy-forward-by-system parameter)",
	  .core_flag = "--proxy-forward-by-system", .core_emit = ET_E_YES },

	{ .name = "accept_dns", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable Magic DNS", .tab = "privacy",
	  .help = "Access other nodes using domain names, modifies your system DNS settings (--accept-dns parameter)",
	  .core_flag = "--accept-dns", .core_emit = ET_E_YESVAL },

	{ .name = "private_mode", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable Private Mode", .tab = "privacy",
	  .help = "Nodes with a different network name and password are not allowed to handshake or relay (--private-mode parameter)",
	  .core_flag = "--private-mode", .core_emit = ET_E_YESVAL },

	{ .name = "quic_proxy", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable QUIC Proxy", .tab = "privacy",
	  .help = "Proxy tcp streams with QUIC (--enable-quic-proxy parameter)",
	  .core_flag = "--enable-quic-proxy", .core_emit = ET_E_YES },

	{ .name = "quic_input", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable QUIC Input", .tab = "privacy",
	  .help = "Do not allow other nodes to use QUIC to proxy tcp streams to this node (--disable-quic-input parameter)",
	  .core_flag = "--disable-quic-input", .core_emit = ET_E_YES },

	{ .name = "foreign_relay_bps_limit", .kind = ET_K_INT, .def = NULL,
	  .label = "Forwarding Rate", .tab = "privacy",
	  .help = "Maximum bps limit for foreign network relay, unit BPS (--foreign-relay-bps-limit parameter)",
	  .core_flag = "--foreign-relay-bps-limit", .core_emit = ET_E_VAL },

	/* 非空且不等于 aes-gcm（默认算法）时才输出 */
	{ .name = "encryption_algorithm", .kind = ET_K_ENUM, .def = "aes-gcm",
	  .label = "Encryption Algorithm", .tab = "privacy",
	  .help = "Encryption algorithm, default is aes-gcm and is not passed explicitly (--encryption-algorithm parameter)",
	  .enum_vals = "xor,chacha20,aes-gcm,aes-256-gcm,openssl-aes-gcm,openssl-chacha20,openssl-aes-256-gcm",
	  .core_flag = "--encryption-algorithm", .core_emit = ET_E_CUSTOM },

	{ .name = "p2p_only", .kind = ET_K_BOOL, .def = "0",
	  .label = "P2P only", .tab = "privacy",
	  .help = "Only communicate with peers that already establish p2p connection (--p2p-only parameter)",
	  .core_flag = "--p2p-only", .core_emit = ET_E_YES },

	{ .name = "disable_tcp", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable TCP", .tab = "privacy",
	  .help = "Disable TCP hole punching (--disable-tcp-hole-punching parameter)",
	  .core_flag = "--disable-tcp-hole-punching", .core_emit = ET_E_YES },

	{ .name = "disable_udp", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable UDP", .tab = "privacy",
	  .help = "Disable UDP hole punching (--disable-udp-hole-punching parameter)",
	  .core_flag = "--disable-udp-hole-punching", .core_emit = ET_E_YES },

	{ .name = "disable_sym", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable sym", .tab = "privacy",
	  .help = "Disable UDP NAT hole punching for symmetric NAT (--disable-sym-hole-punching parameter)",
	  .core_flag = "--disable-sym-hole-punching", .core_emit = ET_E_YES },

	{ .name = "disable_relay_kcp", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable relay kcp", .tab = "privacy",
	  .help = "Disable relay kcp packets (--disable-relay-kcp parameter)",
	  .core_flag = "--disable-relay-kcp", .core_emit = ET_E_YESVAL },

	{ .name = "relay_kcp", .kind = ET_K_BOOL, .def = "0",
	  .label = "Relay foreign network kcp", .tab = "privacy",
	  .help = "Allow relaying kcp packets from foreign networks (--enable-relay-foreign-network-kcp parameter)",
	  .core_flag = "--enable-relay-foreign-network-kcp", .core_emit = ET_E_YESVAL },

	{ .name = "disable_relay_quic", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable relay quic", .tab = "privacy",
	  .help = "Disable relaying quic packets (--disable-relay-quic parameter)",
	  .core_flag = "--disable-relay-quic", .core_emit = ET_E_YESVAL },

	{ .name = "relay_quic", .kind = ET_K_BOOL, .def = "0",
	  .label = "Relay foreign network quic", .tab = "privacy",
	  .help = "Allow relaying quic packets from foreign networks (--enable-relay-foreign-network-quic parameter)",
	  .core_flag = "--enable-relay-foreign-network-quic", .core_emit = ET_E_YESVAL },

	{ .name = "udp_white_port", .kind = ET_K_STR, .def = NULL,
	  .label = "UDP whitelist", .tab = "privacy",
	  .help = "UDP port whitelist, supports single ports and ranges (--udp-whitelist parameter)",
	  .core_flag = "--udp-whitelist", .core_emit = ET_E_VAL },

	{ .name = "tcp_white_port", .kind = ET_K_STR, .def = NULL,
	  .label = "TCP whitelist", .tab = "privacy",
	  .help = "TCP port whitelist, supports single ports and ranges (--tcp-whitelist parameter)",
	  .core_flag = "--tcp-whitelist", .core_emit = ET_E_VAL },

	{ .name = "tld_dns_zone", .kind = ET_K_STR, .def = NULL,
	  .label = "Magic DNS Zone", .tab = "privacy",
	  .help = "Top level DNS zone used by Magic DNS, default is et.net (--tld-dns-zone parameter)",
	  .core_flag = "--tld-dns-zone", .core_emit = ET_E_VAL },

	{ .name = "instance_recv_bps_limit", .kind = ET_K_INT, .def = NULL,
	  .label = "Instance Receive Rate Limit", .tab = "privacy",
	  .help = "Maximum inbound bps limit for this network instance, unit BPS (--instance-recv-bps-limit parameter)",
	  .core_flag = "--instance-recv-bps-limit", .core_emit = ET_E_VAL },

	{ .name = "socket_mark", .kind = ET_K_INT, .def = NULL,
	  .label = "Socket Mark", .tab = "privacy",
	  .help = "Set SO_MARK on all underlay sockets, Linux only (--socket-mark parameter)",
	  .core_flag = "--socket-mark", .core_emit = ET_E_VAL },

	{ .name = "disable_upnp", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable UPnP", .tab = "privacy",
	  .help = "Turn off automatic UPnP/NAT-PMP port mapping (--disable-upnp parameter)",
	  .core_flag = "--disable-upnp", .core_emit = ET_E_YES },

	{ .name = "lazy_p2p", .kind = ET_K_BOOL, .def = "0",
	  .label = "Lazy P2P", .tab = "privacy",
	  .help = "Only try to establish P2P connections when traffic needs to be forwarded (--lazy-p2p parameter)",
	  .core_flag = "--lazy-p2p", .core_emit = ET_E_YES },

	{ .name = "need_p2p", .kind = ET_K_BOOL, .def = "0",
	  .label = "Need P2P", .tab = "privacy",
	  .help = "Require a P2P connection before forwarding traffic through relays (--need-p2p parameter)",
	  .core_flag = "--need-p2p", .core_emit = ET_E_YES },

	{ .name = "stun_servers", .kind = ET_K_LIST, .def = NULL,
	  .label = "STUN Servers", .tab = "privacy",
	  .help = "Custom UDP STUN servers used for NAT type detection and hole punching (--stun-servers parameter)",
	  .core_flag = "--stun-servers", .core_emit = ET_E_REPEAT },

	{ .name = "tcp_stun_servers", .kind = ET_K_LIST, .def = NULL,
	  .label = "STUN Servers (TCP)", .tab = "privacy",
	  .help = "Custom TCP STUN servers used for NAT type detection and hole punching (--tcp-stun-servers parameter)",
	  .core_flag = "--tcp-stun-servers", .core_emit = ET_E_REPEAT },

	/* extra_args 由 et_argv_core() 在最后逐行追加（行内按空白切分），
	 * 数据实际存放在 et_cfg.extra_args，et_cfg_list("extra_args") 可取得 */
	{ .name = "extra_args", .kind = ET_K_LIST, .def = NULL,
	  .label = "Extra Parameters", .tab = "privacy",
	  .help = "Additional command-line arguments passed to the backend process, split on whitespace",
	  .core_flag = NULL, .core_emit = ET_E_NONE },

	/* --console-log-level 不在此处拼装：由调用方追加（两种模式都追加） */
	{ .name = "log", .kind = ET_K_ENUM, .def = "off",
	  .label = "Program Log", .tab = "general",
	  .help = "Runtime log level: Error < Warning < Info < Debug < Trace, 'off' means no --console-log-level",
	  .enum_vals = "off,error,warn,info,debug,trace",
	  .core_flag = "--console-log-level", .core_emit = ET_E_NONE },

	/* ================= 仅 UI / 由 daemon 使用的选项 ================= */
	{ .name = "web_config", .kind = ET_K_STR, .def = NULL,
	  .label = "Web Server Address", .tab = "general",
	  .help = "Web configuration server address (-w parameter). Since EasyTier 2.7.0 a full URL is required, "
		  "e.g. udp://config-server.easytier.cn:22020/username",
	  .core_flag = "-w", .core_emit = ET_E_CUSTOM },

	{ .name = "etcmd", .kind = ET_K_ENUM, .def = "etcmd",
	  .label = "Startup Method", .tab = "general",
	  .help = "Default / Configuration File / Web Configuration",
	  .enum_vals = "etcmd,config,web" },

	{ .name = "enabled", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable", .tab = "general",
	  .help = "Start easytier-core on boot" },

	{ .name = "easytierbin", .kind = ET_K_STR, .def = "/usr/bin/easytier-core",
	  .label = "Easytier Core Path", .tab = "system",
	  .help = "Path of the easytier-core binary" },

	{ .name = "webbin", .kind = ET_K_STR, .def = "/usr/bin/easytier-web",
	  .label = "Easytier Web Path", .tab = "system",
	  .help = "Path of the easytier-web-embed binary" },

	{ .name = "check", .kind = ET_K_BOOL, .def = "0",
	  .label = "Connectivity Check", .tab = "privacy",
	  .help = "Restart EasyTier when all specified IPs fail to ping" },

	{ .name = "checkip", .kind = ET_K_LIST, .def = NULL,
	  .label = "Check IPs", .tab = "privacy",
	  .help = "Remote device IPs used by the connectivity check" },

	{ .name = "checktime", .kind = ET_K_INT, .def = NULL,
	  .label = "Interval Time (minutes)", .tab = "privacy",
	  .help = "Interval time for the connectivity check, in minutes" },

	{ .name = "fallback_version", .kind = ET_K_STR, .def = "v2.7.0",
	  .label = "Fallback Version", .tab = "system",
	  .help = "Version used when the latest release cannot be queried" },

	{ .name = "github_proxys", .kind = ET_K_LIST, .def = NULL,
	  .label = "GitHub Proxies", .tab = "system",
	  .help = "Download mirrors tried in order, empty means the built-in default list" },

	{ .name = "auto_config_interface", .kind = ET_K_BOOL, .def = "1",
	  .label = "Auto Configure Interface", .tab = "privacy",
	  .help = "Automatically create and configure the EasyTier network interface" },

	{ .name = "interface_netmask", .kind = ET_K_STR, .def = "255.0.0.0",
	  .label = "Interface Netmask", .tab = "privacy",
	  .help = "Subnet mask for the EasyTier interface (default: 255.0.0.0)" },

	{ .name = "auto_config_firewall", .kind = ET_K_BOOL, .def = "1",
	  .label = "Auto Configure Firewall", .tab = "privacy",
	  .help = "Automatically add and manage firewall rules" },

	{ .name = "et_forward", .kind = ET_K_LIST, .def = NULL,
	  .label = "Access Control", .tab = "privacy",
	  .help = "Traffic permission rules between network zones: etfwlan, etfwwan, lanfwet, wanfwet" },

	/* ================= easytier-web（start_web，顺序 = 发射顺序） ================= */
	{ .name = "web_db_path", .kind = ET_K_STR, .def = "/etc/easytier/et.db",
	  .label = "Database File Path", .tab = "web",
	  .help = "SQLite database path (-d parameter)",
	  .web_flag = "-d", .web_emit = ET_E_VAL },

	{ .name = "web_protocol", .kind = ET_K_ENUM, .def = "udp",
	  .label = "Listening Protocol", .tab = "web",
	  .help = "Listening protocol of the web console (-p parameter)",
	  .enum_vals = "udp,tcp",
	  .web_flag = "-p", .web_emit = ET_E_VAL },

	{ .name = "web_port", .kind = ET_K_INT, .def = "22020",
	  .label = "Server Port", .tab = "web",
	  .help = "Service port of the web console (-c parameter)",
	  .web_flag = "-c", .web_emit = ET_E_VAL },

	{ .name = "web_api_port", .kind = ET_K_INT, .def = "11211",
	  .label = "API Port", .tab = "web",
	  .help = "API listen port (-a parameter)",
	  .web_flag = "-a", .web_emit = ET_E_VAL },

	{ .name = "web_api_host", .kind = ET_K_STR, .def = NULL,
	  .label = "Default API Server URL", .tab = "web",
	  .help = "Default API server URL returned to clients (--api-host parameter)",
	  .web_flag = "--api-host", .web_emit = ET_E_VAL },

	{ .name = "web_geoip_db", .kind = ET_K_STR, .def = NULL,
	  .label = "GEOIP Database Path", .tab = "web",
	  .help = "GeoIP database path (--geoip-db parameter)",
	  .web_flag = "--geoip-db", .web_emit = ET_E_VAL },

	/* 空则 --no-web，否则 -l <port>。
	 * def 为 NULL：init.d 对 html_port 没有兜底，未配置时走 `[ -z ]` 的 --no-web 分支；
	 * get_web_config 给前端的是 "11211"（表单初值），两者不是一回事，故不写进 argv 默认值。 */
	{ .name = "web_html_port", .kind = ET_K_INT, .def = NULL,
	  .label = "Web Interface Port", .tab = "web",
	  .help = "Web interface port, empty disables the built-in web UI (--no-web / -l parameter)",
	  .web_flag = "-l", .web_emit = ET_E_CUSTOM },

	/* 0.0.0.0（或空）不输出 */
	{ .name = "web_api_addr", .kind = ET_K_STR, .def = "0.0.0.0",
	  .label = "API Listen Address", .tab = "web",
	  .help = "API listen address, 0.0.0.0 means all interfaces and is not passed explicitly (--api-server-addr parameter)",
	  .web_flag = "--api-server-addr", .web_emit = ET_E_CUSTOM },

	{ .name = "web_html_addr", .kind = ET_K_STR, .def = "0.0.0.0",
	  .label = "Web Listen Address", .tab = "web",
	  .help = "Web listen address, 0.0.0.0 means all interfaces and is not passed explicitly (--web-server-addr parameter)",
	  .web_flag = "--web-server-addr", .web_emit = ET_E_CUSTOM },

	{ .name = "web_disable_registration", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable User Registration", .tab = "web",
	  .help = "Disable user registration (--disable-registration parameter)",
	  .web_flag = "--disable-registration", .web_emit = ET_E_ONE },

	{ .name = "web_allow_auto_create_user", .kind = ET_K_BOOL, .def = "0",
	  .label = "Allow Auto-create User", .tab = "web",
	  .help = "Allow auto create user when connecting (--allow-auto-create-user parameter)",
	  .web_flag = "--allow-auto-create-user", .web_emit = ET_E_ONE },

	{ .name = "web_heartbeat_min_response_ms", .kind = ET_K_INT, .def = NULL,
	  .label = "Heartbeat Minimum Response Time (ms)", .tab = "web",
	  .help = "Minimum heartbeat response time in milliseconds (--heartbeat-min-response-ms parameter)",
	  .web_flag = "--heartbeat-min-response-ms", .web_emit = ET_E_VAL },

	{ .name = "web_heartbeat_timeout_ms", .kind = ET_K_INT, .def = NULL,
	  .label = "Heartbeat Timeout (ms)", .tab = "web",
	  .help = "Heartbeat timeout in milliseconds (--heartbeat-timeout-ms parameter)",
	  .web_flag = "--heartbeat-timeout-ms", .web_emit = ET_E_VAL },

	{ .name = "web_gateway_peer_url", .kind = ET_K_STR, .def = NULL,
	  .label = "Gateway Peer URL", .tab = "web",
	  .help = "Peer URL announced by the gateway (--gateway-peer-url parameter)",
	  .web_flag = "--gateway-peer-url", .web_emit = ET_E_VAL },

	{ .name = "web_gateway_relay_data", .kind = ET_K_BOOL, .def = "0",
	  .label = "Gateway Relay Data", .tab = "web",
	  .help = "Relay data through the gateway (--gateway-relay-data true parameter)",
	  .web_flag = "--gateway-relay-data", .web_emit = ET_E_ONEVAL },

	/* OIDC 组：受 web_oidc_enabled 开关控制（见 et_argv_web 的门控逻辑） */
	{ .name = "web_oidc_enabled", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable OIDC Single Sign-On", .tab = "web",
	  .help = "Enable OIDC single sign-on for the web console" },

	{ .name = "web_oidc_issuer_url", .kind = ET_K_STR, .def = NULL,
	  .label = "OIDC Issuer URL *", .tab = "web",
	  .help = "OIDC issuer URL (--oidc-issuer-url parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-issuer-url", .web_emit = ET_E_VAL },

	{ .name = "web_oidc_client_id", .kind = ET_K_STR, .def = NULL,
	  .label = "OIDC Client ID *", .tab = "web",
	  .help = "OIDC client id (--oidc-client-id parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-client-id", .web_emit = ET_E_VAL },

	{ .name = "web_oidc_client_secret", .kind = ET_K_PASS, .def = NULL,
	  .label = "OIDC Client Secret *", .tab = "web",
	  .help = "OIDC client secret (--oidc-client-secret parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-client-secret", .web_emit = ET_E_VAL },

	{ .name = "web_oidc_redirect_url", .kind = ET_K_STR, .def = NULL,
	  .label = "OIDC Redirect URL *", .tab = "web",
	  .help = "OIDC redirect URL (--oidc-redirect-url parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-redirect-url", .web_emit = ET_E_VAL },

	{ .name = "web_oidc_username_claim", .kind = ET_K_STR, .def = "preferred_username",
	  .label = "OIDC Username Claim", .tab = "web",
	  .help = "OIDC username claim (--oidc-username-claim parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-username-claim", .web_emit = ET_E_VAL },

	{ .name = "web_oidc_scopes", .kind = ET_K_STR, .def = "openid profile",
	  .label = "OIDC Scopes", .tab = "web",
	  .help = "OIDC scopes, passed as a single argument (--oidc-scopes parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-scopes", .web_emit = ET_E_VAL },

	{ .name = "web_oidc_frontend_base_url", .kind = ET_K_STR, .def = NULL,
	  .label = "OIDC Frontend Base URL", .tab = "web",
	  .help = "OIDC frontend base URL (--oidc-frontend-base-url parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-frontend-base-url", .web_emit = ET_E_VAL },

	{ .name = "web_oidc_disable_pkce", .kind = ET_K_BOOL, .def = "0",
	  .label = "Disable PKCE", .tab = "web",
	  .help = "Disable PKCE for OIDC (--oidc-disable-pkce parameter, requires OIDC enabled)",
	  .web_flag = "--oidc-disable-pkce", .web_emit = ET_E_ONE },

	/* Webhook 组：受 web_webhook_enabled 开关控制 */
	{ .name = "web_webhook_enabled", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable Webhook Integration", .tab = "web",
	  .help = "Enable webhook integration for the web console" },

	{ .name = "web_webhook_url", .kind = ET_K_STR, .def = NULL,
	  .label = "Webhook URL", .tab = "web",
	  .help = "Webhook URL (--webhook-url parameter, requires webhook enabled)",
	  .web_flag = "--webhook-url", .web_emit = ET_E_VAL },

	{ .name = "web_webhook_secret", .kind = ET_K_PASS, .def = NULL,
	  .label = "Webhook Secret", .tab = "web",
	  .help = "Webhook secret (--webhook-secret parameter, requires webhook enabled)",
	  .web_flag = "--webhook-secret", .web_emit = ET_E_VAL },

	{ .name = "web_internal_auth_token", .kind = ET_K_PASS, .def = NULL,
	  .label = "Internal Auth Token", .tab = "web",
	  .help = "Internal auth token (--internal-auth-token parameter, requires webhook enabled)",
	  .web_flag = "--internal-auth-token", .web_emit = ET_E_VAL },

	{ .name = "web_web_instance_id", .kind = ET_K_STR, .def = NULL,
	  .label = "Web Instance ID", .tab = "web",
	  .help = "Web instance id (--web-instance-id parameter, requires webhook enabled)",
	  .web_flag = "--web-instance-id", .web_emit = ET_E_VAL },

	{ .name = "web_web_instance_api_base_url", .kind = ET_K_STR, .def = NULL,
	  .label = "Web Instance API Base URL", .tab = "web",
	  .help = "Web instance API base URL (--web-instance-api-base-url parameter, requires webhook enabled)",
	  .web_flag = "--web-instance-api-base-url", .web_emit = ET_E_VAL },

	/* 仅 UI：web 控制台的日志级别（默认 off，off 时不输出 --console-log-level） */
	{ .name = "web_weblog", .kind = ET_K_ENUM, .def = "off",
	  .label = "Log Level", .tab = "web",
	  .help = "Log level of easytier-web, 'off' means no --console-log-level",
	  .enum_vals = "off,error,warn,info,debug,trace",
	  .web_flag = "--console-log-level", .web_emit = ET_E_CUSTOM },

	/* 仅 UI：web 防火墙自动化开关（由 daemon 的 set_firewall 逻辑使用） */
	{ .name = "web_fw_web", .kind = ET_K_BOOL, .def = "0",
	  .label = "WAN access to WEB", .tab = "web",
	  .help = "Open the web service port to WAN" },

	{ .name = "web_fw_api", .kind = ET_K_BOOL, .def = "0",
	  .label = "WAN access to API", .tab = "web",
	  .help = "Open the API port to WAN" },

	{ .name = "web_enabled", .kind = ET_K_BOOL, .def = "0",
	  .label = "Enable Self-hosted Web Console", .tab = "web",
	  .help = "Start easytier-web-embed on boot" },
};

const size_t et_schema_len = sizeof(et_schema) / sizeof(et_schema[0]);

const et_field *et_schema_find(const char *name)
{
	if (!name)
		return NULL;
	for (size_t i = 0; i < et_schema_len; i++) {
		if (et_schema[i].name && strcmp(et_schema[i].name, name) == 0)
			return &et_schema[i];
	}
	return NULL;
}
