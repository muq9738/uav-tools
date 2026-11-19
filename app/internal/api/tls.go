package api

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"fmt"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"time"
)

// LoadOrGenerateTLSConfig 加载指定的 PEM 证书与私钥，或自动生成并持久化自签 ECDSA 证书。
// 直接使用 Go 标准库 crypto/tls，无需额外打包 Caddy/Nginx 外部进程。
func LoadOrGenerateTLSConfig(certFile, keyFile, dataDir, addr string) (*tls.Config, string, error) {
	if (certFile == "") != (keyFile == "") {
		return nil, "", errors.New("--tls-cert 与 --tls-key 必须同时指定")
	}
	if certFile != "" {
		cert, err := tls.LoadX509KeyPair(certFile, keyFile)
		if err != nil {
			return nil, "", fmt.Errorf("加载 TLS 证书失败：%w", err)
		}
		return newTLSConfig(cert), certFile, nil
	}

	if dataDir != "" {
		tlsDir := filepath.Join(dataDir, "tls")
		savedCert := filepath.Join(tlsDir, "cert.pem")
		savedKey := filepath.Join(tlsDir, "key.pem")
		if cert, ok := loadValidCertificate(savedCert, savedKey); ok {
			return newTLSConfig(cert), savedCert, nil
		}
		certPEM, keyPEM, err := generateSelfSignedPEM(addr)
		if err != nil {
			return nil, "", err
		}
		if err := os.MkdirAll(tlsDir, 0o755); err != nil {
			return nil, "", fmt.Errorf("创建 TLS 证书目录失败：%w", err)
		}
		if err := os.WriteFile(savedCert, certPEM, 0o644); err != nil {
			return nil, "", fmt.Errorf("写入自签证书失败：%w", err)
		}
		if err := os.WriteFile(savedKey, keyPEM, 0o600); err != nil {
			return nil, "", fmt.Errorf("写入自签私钥失败：%w", err)
		}
		cert, err := tls.X509KeyPair(certPEM, keyPEM)
		if err != nil {
			return nil, "", err
		}
		return newTLSConfig(cert), savedCert, nil
	}

	certPEM, keyPEM, err := generateSelfSignedPEM(addr)
	if err != nil {
		return nil, "", err
	}
	cert, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return nil, "", err
	}
	return newTLSConfig(cert), "(in-memory self-signed)", nil
}

func newTLSConfig(cert tls.Certificate) *tls.Config {
	return &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	}
}

func loadValidCertificate(certFile, keyFile string) (tls.Certificate, bool) {
	cert, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil || len(cert.Certificate) == 0 {
		return tls.Certificate{}, false
	}
	leaf, err := x509.ParseCertificate(cert.Certificate[0])
	if err != nil {
		return tls.Certificate{}, false
	}
	// 距离过期不足 30 天则重新签发。
	if time.Until(leaf.NotAfter) < 30*24*time.Hour {
		return tls.Certificate{}, false
	}
	return cert, true
}

func generateSelfSignedPEM(addr string) ([]byte, []byte, error) {
	privateKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, fmt.Errorf("生成 TLS 私钥失败：%w", err)
	}
	serialLimit := new(big.Int).Lsh(big.NewInt(1), 128)
	serial, err := rand.Int(rand.Reader, serialLimit)
	if err != nil {
		return nil, nil, fmt.Errorf("生成证书序列号失败：%w", err)
	}

	dnsNames := []string{"localhost"}
	if hostname, err := os.Hostname(); err == nil && hostname != "" && hostname != "localhost" {
		dnsNames = append(dnsNames, hostname)
	}
	ipSet := map[string]net.IP{
		"127.0.0.1": net.ParseIP("127.0.0.1"),
		"::1":       net.ParseIP("::1"),
	}
	if host, _, err := net.SplitHostPort(addr); err == nil && host != "" {
		if ip := net.ParseIP(host); ip != nil && !ip.IsUnspecified() {
			ipSet[ip.String()] = ip
		} else if ip == nil && host != "localhost" {
			dnsNames = append(dnsNames, host)
		}
	}
	if addrs, err := net.InterfaceAddrs(); err == nil {
		for _, a := range addrs {
			if ipNet, ok := a.(*net.IPNet); ok && ipNet.IP != nil && !ipNet.IP.IsUnspecified() {
				ipSet[ipNet.IP.String()] = ipNet.IP
			}
		}
	}
	ips := make([]net.IP, 0, len(ipSet))
	for _, ip := range ipSet {
		ips = append(ips, ip)
	}

	now := time.Now()
	template := &x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			CommonName:   "osgb-tiles-app",
			Organization: []string{"osgb-tiles-app"},
		},
		NotBefore:             now.Add(-time.Hour),
		NotAfter:              now.AddDate(10, 0, 0),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              dnsNames,
		IPAddresses:           ips,
	}

	derBytes, err := x509.CreateCertificate(rand.Reader, template, template, &privateKey.PublicKey, privateKey)
	if err != nil {
		return nil, nil, fmt.Errorf("签发自签证书失败：%w", err)
	}
	keyBytes, err := x509.MarshalECPrivateKey(privateKey)
	if err != nil {
		return nil, nil, fmt.Errorf("编码 TLS 私钥失败：%w", err)
	}

	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: derBytes})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyBytes})
	return certPEM, keyPEM, nil
}
