// Copyright (C) 2026  Mo3he
// SPDX-License-Identifier: AGPL-3.0-or-later

// Userspace netstack + proxy sidecar for the OpenVPN ACAP.
//
// The OpenVPN3 client (tun_probe, C++) terminates the tunnel in userspace and
// hands us one end of a SOCK_DGRAM socketpair carrying decrypted IP packets
// (one datagram == one IP packet), passed as inherited file descriptor 3. This
// process attaches a gVisor netstack to that fd (via wireguard's netstack
// helper, reused purely for its stack setup) and runs the same proxy/forwarder
// layer as the WireGuard ACAP:
//
//   - Transparent TCP forwarders for camera ports 80/443/554 (VPN peer -> camera)
//   - Inbound SOCKS5 on <vpn-ip>:1080 (VPN peer -> any camera port)
//   - Outbound HTTP CONNECT on 127.0.0.1:8080 (camera -> VPN/internet)
//   - Outbound SOCKS5 on 127.0.0.1:1080 (camera -> VPN/internet)
//
// No kernel TUN, no root: the "tun" is just the socketpair fd.
//
// Args: netstack_proxy <tun_fd> <client_cidr> <mtu> <http_port> <socks_port>

package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"log/syslog"
	"net"
	"net/netip"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.zx2c4.com/wireguard/tun"
	"golang.zx2c4.com/wireguard/tun/netstack"
)

// transparentPorts are forwarded directly: VPN-IP:port -> 127.0.0.1:port.
var transparentPorts = []int{80, 443, 554}

// inboundSOCKS5Port is the SOCKS5 port on the VPN interface (netstack side).
const inboundSOCKS5Port = 1080

const pumpOffset = 0

type tunnel struct {
	tnet   *netstack.Net
	stopCh chan struct{}
	wg     sync.WaitGroup
}

func parsePort(s string, def int) int {
	if p, err := strconv.Atoi(s); err == nil && p > 0 && p < 65536 {
		return p
	}
	return def
}

// pump copies IP packets between the OpenVPN socketpair fd and the netstack tun
// device. fd -> device is inbound (decrypted packets into the stack); device ->
// fd is outbound (stack packets to be encrypted by OpenVPN).
func (t *tunnel) pump(dev tun.Device, fd *os.File, mtu int) {
	// inbound: fd -> netstack
	t.wg.Add(1)
	go func() {
		defer t.wg.Done()
		buf := make([]byte, mtu+256)
		for {
			n, err := fd.Read(buf)
			if err != nil {
				select {
				case <-t.stopCh:
				default:
					slog.Error("tun fd read", "err", err)
				}
				return
			}
			if n <= 0 {
				continue
			}
			pkt := make([]byte, n)
			copy(pkt, buf[:n])
			if _, err := dev.Write([][]byte{pkt}, pumpOffset); err != nil {
				slog.Warn("netstack write", "err", err)
			}
		}
	}()

	// outbound: netstack -> fd
	t.wg.Add(1)
	go func() {
		defer t.wg.Done()
		bufs := [][]byte{make([]byte, mtu+256)}
		sizes := make([]int, 1)
		for {
			k, err := dev.Read(bufs, sizes, pumpOffset)
			if err != nil {
				select {
				case <-t.stopCh:
				default:
					slog.Error("netstack read", "err", err)
				}
				return
			}
			for i := 0; i < k; i++ {
				if _, err := fd.Write(bufs[i][pumpOffset : pumpOffset+sizes[i]]); err != nil {
					slog.Warn("tun fd write", "err", err)
				}
			}
		}
	}()
}

// runTCPProxy listens on localAddr:port inside the netstack and forwards each
// accepted connection to dstAddr on the host.
func (t *tunnel) runTCPProxy(localAddr netip.Addr, port int, dstAddr string) {
	defer t.wg.Done()
	listenAddr := net.TCPAddrFromAddrPort(netip.AddrPortFrom(localAddr, uint16(port)))
	ln, err := t.tnet.ListenTCP(listenAddr)
	if err != nil {
		slog.Error("proxy listen", "port", port, "err", err)
		return
	}
	go func() { <-t.stopCh; _ = ln.Close() }()
	slog.Info("transparent forwarder ready", "listen", listenAddr, "dst", dstAddr)
	for {
		c, err := ln.Accept()
		if err != nil {
			select {
			case <-t.stopCh:
				return
			default:
				time.Sleep(time.Second)
				continue
			}
		}
		go relay(c, dstAddr)
	}
}

func relay(src net.Conn, dst string) {
	defer src.Close()
	dstConn, err := net.DialTimeout("tcp", dst, 10*time.Second)
	if err != nil {
		return
	}
	defer dstConn.Close()
	done := make(chan struct{}, 2)
	go func() { _, _ = io.Copy(dstConn, src); done <- struct{}{} }()
	go func() { _, _ = io.Copy(src, dstConn); done <- struct{}{} }()
	<-done
}

// runInboundSOCKS5 listens on the VPN IP and forwards SOCKS5 CONNECT to
// 127.0.0.1:<requested-port> on the host (VPN peer -> any camera port).
func (t *tunnel) runInboundSOCKS5(localAddr netip.Addr, port int) {
	defer t.wg.Done()
	listenAddr := net.TCPAddrFromAddrPort(netip.AddrPortFrom(localAddr, uint16(port)))
	ln, err := t.tnet.ListenTCP(listenAddr)
	if err != nil {
		slog.Error("inbound socks5 listen", "err", err)
		return
	}
	go func() { <-t.stopCh; _ = ln.Close() }()
	slog.Info("inbound SOCKS5 ready", "addr", listenAddr)
	for {
		c, err := ln.Accept()
		if err != nil {
			select {
			case <-t.stopCh:
				return
			default:
				time.Sleep(time.Second)
				continue
			}
		}
		go handleInboundSOCKS5(c)
	}
}

// handleInboundSOCKS5 implements SOCKS5 CONNECT (RFC 1928); the destination host
// is always replaced with 127.0.0.1 so it can only reach local camera services.
func handleInboundSOCKS5(c net.Conn) {
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(30 * time.Second))
	buf := make([]byte, 257)
	if _, err := io.ReadFull(c, buf[:2]); err != nil || buf[0] != 0x05 {
		return
	}
	nmethods := int(buf[1])
	if _, err := io.ReadFull(c, buf[:nmethods]); err != nil {
		return
	}
	_, _ = c.Write([]byte{0x05, 0x00})
	if _, err := io.ReadFull(c, buf[:4]); err != nil {
		return
	}
	if buf[0] != 0x05 || buf[1] != 0x01 {
		_, _ = c.Write([]byte{0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	var port uint16
	switch buf[3] {
	case 0x01:
		if _, err := io.ReadFull(c, buf[:6]); err != nil {
			return
		}
		port = binary.BigEndian.Uint16(buf[4:6])
	case 0x03:
		if _, err := io.ReadFull(c, buf[:1]); err != nil {
			return
		}
		nameLen := int(buf[0])
		if _, err := io.ReadFull(c, buf[:nameLen+2]); err != nil {
			return
		}
		port = binary.BigEndian.Uint16(buf[nameLen : nameLen+2])
	case 0x04:
		if _, err := io.ReadFull(c, buf[:18]); err != nil {
			return
		}
		port = binary.BigEndian.Uint16(buf[16:18])
	default:
		_, _ = c.Write([]byte{0x05, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	dst := fmt.Sprintf("127.0.0.1:%d", port)
	_ = c.SetDeadline(time.Time{})
	dstConn, err := net.DialTimeout("tcp", dst, 10*time.Second)
	if err != nil {
		_, _ = c.Write([]byte{0x05, 0x04, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	defer dstConn.Close()
	_, _ = c.Write([]byte{0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, byte(port >> 8), byte(port)})
	done := make(chan struct{}, 2)
	go func() { _, _ = io.Copy(dstConn, c); done <- struct{}{} }()
	go func() { _, _ = io.Copy(c, dstConn); done <- struct{}{} }()
	<-done
}

// dialViaVPN resolves hostport via the host DNS resolver, then connects through
// the netstack so traffic exits via the VPN tunnel.
func (t *tunnel) dialViaVPN(ctx context.Context, hostport string) (net.Conn, error) {
	host, port, err := net.SplitHostPort(hostport)
	if err != nil {
		return nil, err
	}
	if ip := net.ParseIP(host); ip == nil {
		addrs, err := net.DefaultResolver.LookupHost(ctx, host)
		if err != nil || len(addrs) == 0 {
			return nil, fmt.Errorf("resolve %s: %w", host, err)
		}
		host = addrs[0]
	}
	return t.tnet.DialContext(ctx, "tcp", net.JoinHostPort(host, port))
}

// runHTTPProxy listens on 127.0.0.1:port (host) and handles HTTP CONNECT (and
// plain HTTP) by tunnelling through the netstack (camera -> VPN/internet).
func (t *tunnel) runHTTPProxy(port int) {
	defer t.wg.Done()
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		slog.Error("http proxy listen failed", "port", port, "err", err)
		return
	}
	go func() { <-t.stopCh; _ = ln.Close() }()
	slog.Info("HTTP CONNECT proxy ready", "addr", ln.Addr())
	for {
		c, err := ln.Accept()
		if err != nil {
			select {
			case <-t.stopCh:
				return
			default:
				time.Sleep(time.Second)
				continue
			}
		}
		go t.handleHTTPProxy(c)
	}
}

func (t *tunnel) handleHTTPProxy(c net.Conn) {
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(30 * time.Second))
	rd := bufio.NewReader(c)
	requestLine, err := rd.ReadString('\n')
	if err != nil {
		return
	}
	parts := strings.SplitN(strings.TrimSpace(requestLine), " ", 3)
	if len(parts) != 3 {
		return
	}
	method, target, httpVer := parts[0], parts[1], parts[2]
	if strings.ToUpper(method) == "CONNECT" {
		for {
			line, err := rd.ReadString('\n')
			if err != nil {
				return
			}
			if strings.TrimSpace(line) == "" {
				break
			}
		}
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		upstream, err := t.dialViaVPN(ctx, target)
		cancel()
		if err != nil {
			_, _ = fmt.Fprintf(c, "%s 502 Bad Gateway\r\n\r\n", httpVer)
			return
		}
		defer upstream.Close()
		_ = c.SetDeadline(time.Time{})
		_, _ = fmt.Fprintf(c, "%s 200 Connection established\r\n\r\n", httpVer)
		done := make(chan struct{}, 2)
		go func() { _, _ = io.Copy(upstream, rd); done <- struct{}{} }()
		go func() { _, _ = io.Copy(c, upstream); done <- struct{}{} }()
		<-done
		return
	}
	u, err := url.Parse(target)
	if err != nil {
		_, _ = fmt.Fprintf(c, "%s 400 Bad Request\r\n\r\n", httpVer)
		return
	}
	host := u.Host
	if !strings.Contains(host, ":") {
		host += ":80"
	}
	var headerLines []string
	for {
		line, readErr := rd.ReadString('\n')
		if readErr != nil {
			return
		}
		headerLines = append(headerLines, line)
		if strings.TrimSpace(line) == "" {
			break
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	upstream, err := t.dialViaVPN(ctx, host)
	cancel()
	if err != nil {
		_, _ = fmt.Fprintf(c, "%s 502 Bad Gateway\r\n\r\n", httpVer)
		return
	}
	defer upstream.Close()
	_ = c.SetDeadline(time.Time{})
	_, _ = fmt.Fprintf(upstream, "%s %s %s\r\n", method, u.RequestURI(), httpVer)
	for _, h := range headerLines {
		_, _ = upstream.Write([]byte(h))
	}
	done := make(chan struct{}, 2)
	go func() { _, _ = io.Copy(upstream, rd); done <- struct{}{} }()
	go func() { _, _ = io.Copy(c, upstream); done <- struct{}{} }()
	<-done
}

// runOutboundSOCKS5 listens on 127.0.0.1:port (host) and forwards SOCKS5 CONNECT
// through the netstack (camera services -> VPN/internet).
func (t *tunnel) runOutboundSOCKS5(port int) {
	defer t.wg.Done()
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		slog.Error("outbound socks5 listen failed", "port", port, "err", err)
		return
	}
	go func() { <-t.stopCh; _ = ln.Close() }()
	slog.Info("outbound SOCKS5 ready", "addr", ln.Addr())
	for {
		c, err := ln.Accept()
		if err != nil {
			select {
			case <-t.stopCh:
				return
			default:
				time.Sleep(time.Second)
				continue
			}
		}
		go t.handleOutboundSOCKS5(c)
	}
}

func (t *tunnel) handleOutboundSOCKS5(c net.Conn) {
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(30 * time.Second))
	buf := make([]byte, 257)
	if _, err := io.ReadFull(c, buf[:2]); err != nil || buf[0] != 0x05 {
		return
	}
	nmethods := int(buf[1])
	if _, err := io.ReadFull(c, buf[:nmethods]); err != nil {
		return
	}
	_, _ = c.Write([]byte{0x05, 0x00})
	if _, err := io.ReadFull(c, buf[:4]); err != nil {
		return
	}
	if buf[0] != 0x05 || buf[1] != 0x01 {
		_, _ = c.Write([]byte{0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	var hostport string
	switch buf[3] {
	case 0x01:
		if _, err := io.ReadFull(c, buf[:6]); err != nil {
			return
		}
		ip := net.IP(buf[:4]).String()
		port := binary.BigEndian.Uint16(buf[4:6])
		hostport = net.JoinHostPort(ip, strconv.Itoa(int(port)))
	case 0x03:
		if _, err := io.ReadFull(c, buf[:1]); err != nil {
			return
		}
		nameLen := int(buf[0])
		if _, err := io.ReadFull(c, buf[:nameLen+2]); err != nil {
			return
		}
		name := string(buf[:nameLen])
		port := binary.BigEndian.Uint16(buf[nameLen : nameLen+2])
		hostport = net.JoinHostPort(name, strconv.Itoa(int(port)))
	case 0x04:
		if _, err := io.ReadFull(c, buf[:18]); err != nil {
			return
		}
		ip := net.IP(buf[:16]).String()
		port := binary.BigEndian.Uint16(buf[16:18])
		hostport = net.JoinHostPort(ip, strconv.Itoa(int(port)))
	default:
		_, _ = c.Write([]byte{0x05, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	_ = c.SetDeadline(time.Time{})
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	upstream, err := t.dialViaVPN(ctx, hostport)
	cancel()
	if err != nil {
		_, _ = c.Write([]byte{0x05, 0x04, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	defer upstream.Close()
	_, _ = c.Write([]byte{0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
	done := make(chan struct{}, 2)
	go func() { _, _ = io.Copy(upstream, c); done <- struct{}{} }()
	go func() { _, _ = io.Copy(c, upstream); done <- struct{}{} }()
	<-done
}

func main() {
	if w, err := syslog.New(syslog.LOG_INFO|syslog.LOG_USER, "OpenVPN"); err == nil {
		slog.SetDefault(slog.New(slog.NewTextHandler(w, nil)))
	}

	if len(os.Args) < 6 {
		slog.Error("usage: netstack_proxy <tun_fd> <client_cidr> <mtu> <http_port> <socks_port>")
		os.Exit(2)
	}
	fdNum, _ := strconv.Atoi(os.Args[1])
	clientCIDR := os.Args[2]
	mtu := parsePort(os.Args[3], 1500)
	httpPort := parsePort(os.Args[4], 8080)
	socksOutPort := parsePort(os.Args[5], 1080)

	prefix, err := netip.ParsePrefix(clientCIDR)
	if err != nil {
		slog.Error("invalid client CIDR", "cidr", clientCIDR, "err", err)
		os.Exit(1)
	}
	localAddr := prefix.Addr()

	fd := os.NewFile(uintptr(fdNum), "tun")
	if fd == nil {
		slog.Error("invalid tun fd", "fd", fdNum)
		os.Exit(1)
	}

	dev, tnet, err := netstack.CreateNetTUN([]netip.Addr{localAddr}, []netip.Addr{}, mtu)
	if err != nil {
		slog.Error("create netstack", "err", err)
		os.Exit(1)
	}

	t := &tunnel{tnet: tnet, stopCh: make(chan struct{})}
	t.pump(dev, fd, mtu)

	for _, port := range transparentPorts {
		t.wg.Add(1)
		go t.runTCPProxy(localAddr, port, fmt.Sprintf("127.0.0.1:%d", port))
	}
	t.wg.Add(1)
	go t.runInboundSOCKS5(localAddr, inboundSOCKS5Port)
	t.wg.Add(1)
	go t.runHTTPProxy(httpPort)
	t.wg.Add(1)
	go t.runOutboundSOCKS5(socksOutPort)

	slog.Info("netstack proxy up", "vpn_ip", localAddr.String(), "mtu", mtu)
	t.wg.Wait()
}
