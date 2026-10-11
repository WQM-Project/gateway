import { rootCertificates } from 'node:tls'
import { X509Certificate } from 'node:crypto'
import { writeFileSync } from 'node:fs'

if (process.env.SUPABASE_URL !== 'https://scvcufiagvbeixhnwjze.supabase.co')
  throw new Error('Load the WQM environment only')
const key = process.env.GATEWAY_INGEST_KEY
if (!/^[a-f0-9]{64}$/.test(key || '')) throw new Error('Expected the existing 64-character gateway key')
const root = rootCertificates.find(pem => new X509Certificate(pem).subject.includes('CN=GTS Root R4'))
if (!root) throw new Error('GTS Root R4 not found in Node trusted CA bundle')
writeFileSync(new URL('../firmware/upload.local.h', import.meta.url),
  `#pragma once\n// Device upload credential only; ignored by Git. Never share this file.\nconstexpr const char *GATEWAY_UPLOAD_KEY = "${key}";\n`)
writeFileSync(new URL('../firmware/upload_ca.h', import.meta.url),
  `#pragma once\n// Public GTS Root R4 from Node's trusted CA bundle. Recheck if endpoint issuer changes.\nconstexpr const char *UPLOAD_CA = R"PEM(${root}\n)PEM";\n`)
console.log('Prepared ignored device key and public TLS trust anchor; no credentials printed.')
