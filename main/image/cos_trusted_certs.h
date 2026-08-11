/*
 * cos_trusted_certs.h
 *
 * Embedded CA certificates for Tencent COS HTTPS verification.
 *
 * The Tencent COS server uses:
 *   Leaf: *.cos.ap-guangzhou.myqcloud.com
 *   Intermediate: GlobalSign Atlas R3 OV TLS CA 2026 Q1
 *   Root: GlobalSign
 *
 * The Atlas R3 intermediate is NOT in ESP-IDF v6.0.2's default cert bundle,
 * so we embed both the intermediate and the root CA here as a single PEM blob.
 *
 * Extracted from live TLS handshake on 2026-08-10.
 */

#ifndef COS_TRUSTED_CERTS_H
#define COS_TRUSTED_CERTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* GlobalSign Atlas R3 OV TLS CA 2026 Q1 + GlobalSign Root CA R3 */
static const char COS_CA_CERT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIEkDCCA3igAwIBAgIRAIRx9E+fk3iGs/qiMHFMX8AwDQYJKoZIhvcNAQELBQAw\n"
    "TDEgMB4GA1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkds\n"
    "b2JhbFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMjUxMDE1MDMxMDAyWhcN\n"
    "MjcxMDE1MDAwMDAwWjBYMQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2ln\n"
    "biBudi1zYTEuMCwGA1UEAxMlR2xvYmFsU2lnbiBBdGxhcyBSMyBPViBUTFMgQ0Eg\n"
    "MjAyNiBRMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKCVPu0JMF+1\n"
    "35y1LrJ0PUvo82n9xrVVBRQjmGkypkTa1UIIMwq2GbU96aVVfAAMdpleyWBHp9Oo\n"
    "Oa2WGtfEXY0gljHvesO2e8/6ahvLiwORRqQyHFsuXGDrfVWpLm8tpdNiE8q4HCix\n"
    "V5K8FCKeSm2v2Aq5jefl+OJe7TPyBdPn4sKxa+fWCDWVm8QGM/7HU1U69U5lkrx4\n"
    "ixDBRhuW+JOFnwcxzBMZ+RHf8TFN5Qu+2xM1v2R+bv3IvkAY4sltTQgewUAoDCQz\n"
    "eI2KrvmpiHnoEVDGKg3KYaEdNuacooReoh0hefIbgQcpzIuVFd//FQEE0P2LfY23\n"
    "KIacb4UPWbECAwEAAaOCAV8wggFbMA4GA1UdDwEB/wQEAwIBhjAdBgNVHSUEFjAU\n"
    "BggrBgEFBQcDAQYIKwYBBQUHAwIwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHQ4E\n"
    "FgQU6sS1/o6Nampvo+vxuixa/NrtVWwwHwYDVR0jBBgwFoAUj/BLf6guRSSuTVD6\n"
    "Y5qL3uLdG7wwewYIKwYBBQUHAQEEbzBtMC4GCCsGAQUFBzABhiJodHRwOi8vb2Nz\n"
    "cDIuZ2xvYmFsc2lnbi5jb20vcm9vdHIzMDsGCCsGAQUFBzAChi9odHRwOi8vc2Vj\n"
    "dXJlLmdsb2JhbHNpZ24uY29tL2NhY2VydC9yb290LXIzLmNydDA2BgNVHR8ELzAt\n"
    "MCugKaAnhiVodHRwOi8vY3JsLmdsb2JhbHNpZ24uY29tL3Jvb3QtcjMuY3JsMCEG\n"
    "A1UdIAQaMBgwCAYGZ4EMAQICMAwGCisGAQQBoDIKAQIwDQYJKoZIhvcNAQELBQAD\n"
    "ggEBADtC+KuzEXDbnznM+RmF4MboUNmR8QWb706EmWMHVQgyp4VRoDGQk4YZlP7k\n"
    "4nr7bVmROuie/jviszRTUjdWGzQFMwcLmg/MZ6qhar+9TpoDTRlefo75jZgwX/HK\n"
    "loryPkNOct+1UKY9E/b6lVi/KOqX4Ims4n0fx6t5XMmGk2RcwcErCk+d4n4kwqz6\n"
    "5LVvYw6K4cbu2VICfB32y61Vb0+87WKKO43szT+ybe9XC84lFj9bLSzV9ngXC1TR\n"
    "XtFvuSsi3CaWUSGacPh0u5yCZebBnoHinM21zhrERKt/K/LMYCWgYq1Rz7Xf/8D+\n"
    "UG0mQv9ZD87UhKb/2UhKjmtkXjE=\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIETjCCAzagAwIBAgINAe5fFp3/lzUrZGXWajANBgkqhkiG9w0BAQsFADBXMQsw\n"
    "CQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UECxMH\n"
    "Um9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTE4MDkxOTAw\n"
    "MDAwMFoXDTI4MDEyODEyMDAwMFowTDEgMB4GA1UECxMXR2xvYmFsU2lnbiBSb290\n"
    "IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkdsb2JhbFNp\n"
    "Z24wggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDMJXaQeQZ4Ihb1wIO2\n"
    "hMoonv0FdhHFrYhy/EYCQ8eyip0EXyTLLkvhYIJG4VKrDIFHcGzdZNHr9SyjD4I9\n"
    "DCuul9e2FIYQebs7E4B3jAjhSdJqYi8fXvqWaN+JJ5U4nwbXPsnLJlkNc96wyOkm\n"
    "DoMVxu9bi9IEYMpJpij2aTv2y8gokeWdimFXN6x0FNx04Druci8unPvQu7/1PQDh\n"
    "BjPogiuuU6Y6FnOM3UEOIDrAtKeh6bJPkC4yYOlXy7kEkmho5TgmYHWyn3f/kRTv\n"
    "riBJ/K1AFUjRAjFhGV64l++td7dkmnq/X8ET75ti+w1s4FRpFqkD2m7pg5NxdsZp\n"
    "hYIXAgMBAAGjggEiMIIBHjAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0TAQH/BAUwAwEB\n"
    "/zAdBgNVHQ4EFgQUj/BLf6guRSSuTVD6Y5qL3uLdG7wwHwYDVR0jBBgwFoAUYHtm\n"
    "GkUNl8qJUC99BM00qP/8/UswPQYIKwYBBQUHAQEEMTAvMC0GCCsGAQUFBzABhiFo\n"
    "dHRwOi8vb2NzcC5nbG9iYWxzaWduLmNvbS9yb290cjEwMwYDVR0fBCwwKjAooCag\n"
    "JIYiaHR0cDovL2NybC5nbG9iYWxzaWduLmNvbS9yb290LmNybDBHBgNVHSAEQDA+\n"
    "MDwGBFUdIAAwNDAyBggrBgEFBQcCARYmaHR0cHM6Ly93d3cuZ2xvYmFsc2lnbi5j\n"
    "b20vcmVwb3NpdG9yeS8wDQYJKoZIhvcNAQELBQADggEBACNw6c/ivvVZrpRCb8RD\n"
    "M6rNPzq5ZBfyYgZLSPFAiAYXof6r0V88xjPy847dHx0+zBpgmYILrMf8fpqHKqV9\n"
    "D6ZX7qw7aoXW3r1AY/itpsiIsBL89kHfDwmXHjjqU5++BfQ+6tOfUBJ2vgmLwgtI\n"
    "fR4uUfaNU9OrH0Abio7tfftPeVZwXwzTjhuzp3ANNyuXlava4BJrHEDOxcd+7cJi\n"
    "WOx37XMiwor1hkOIreoTbv3Y/kIvuX1erRjvlJDKPSerJpSZdcfL03v3ykzTr1Eh\n"
    "kluEfSufFT90y1HonoMOFm8b50bOI7355KKL0jlrqnkckSziYSQtjipIcJDEHsXo\n"
    "4HA=\n"
    "-----END CERTIFICATE-----\n";

#ifdef __cplusplus
}
#endif

#endif /* COS_TRUSTED_CERTS_H */
