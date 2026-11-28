#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# pull.py <image ref> <platform> <out dir>: an image of a registry (Docker Hub by default), one platform of it, as
# its manifest, its config and its layers, written into <out dir> (docs/research/tabc-poc.md). The smallest pull
# there is: anonymous, no layer cache, no other registry API. Each blob is checked against its digest. Prints the
# manifest's digest. Example: pull.py alpine:3.24.4 linux/amd64 out/tabc-poc/oci/amd64
import hashlib, json, os, sys, urllib.request

ref, platform, out = sys.argv[1], sys.argv[2], sys.argv[3]
repo, _, tag = ref.partition(':'); tag = tag or 'latest'
if '/' not in repo: repo = 'library/' + repo
os_, _, arch = platform.partition('/')
REG = 'https://registry-1.docker.io'
tok = json.load(urllib.request.urlopen(f'https://auth.docker.io/token?service=registry.docker.io&scope=repository:{repo}:pull'))['token']
LIST = 'application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json'
ONE = 'application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json'

class Redirect(urllib.request.HTTPRedirectHandler):   # a blob's redirect is to a CDN, which refuses the registry's token
    def redirect_request(self, req, fp, code, msg, headers, url):
        return urllib.request.Request(url)
fetch = urllib.request.build_opener(Redirect)

def get(path, accept=None):
    req = urllib.request.Request(REG + path, headers={'Authorization': f'Bearer {tok}', **({'Accept': accept} if accept else {})})
    return fetch.open(req).read()

def blob(digest):
    b = get(f'/v2/{repo}/blobs/{digest}')
    assert 'sha256:' + hashlib.sha256(b).hexdigest() == digest, f'{digest}: the bytes do not match'
    return b

index = json.loads(get(f'/v2/{repo}/manifests/{tag}', LIST + ', ' + ONE))
digest = None
if 'manifests' in index:
    for m in index['manifests']:
        p = m.get('platform', {})
        if p.get('os') == os_ and p.get('architecture') == arch:
            digest = m['digest']; break
    assert digest, f'{ref}: no {platform} in its index'
    raw = get(f'/v2/{repo}/manifests/{digest}', ONE)
else:
    raw = json.dumps(index).encode(); digest = None
assert digest is None or 'sha256:' + hashlib.sha256(raw).hexdigest() == digest
manifest = json.loads(raw)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'manifest.json'), 'wb').write(raw)
open(os.path.join(out, 'config.json'), 'wb').write(blob(manifest['config']['digest']))
layers = []
for i, l in enumerate(manifest['layers']):
    name = f'layer{i}.tar.gz'; open(os.path.join(out, name), 'wb').write(blob(l['digest'])); layers.append(name)
json.dump({'ref': ref, 'platform': platform, 'manifest': digest or 'sha256:' + hashlib.sha256(raw).hexdigest(),
           'layers': [l['digest'] for l in manifest['layers']], 'files': layers}, open(os.path.join(out, 'pulled.json'), 'w'), indent=1)
print(digest or 'sha256:' + hashlib.sha256(raw).hexdigest())
