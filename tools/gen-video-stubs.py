import re, sys
src = open(sys.argv[1]).read()
# struct members
m = re.search(r'typedef struct D3D11_1DDI_VIDEODEVICEFUNCS\s*\{(.*?)\}\s*D3D11_1DDI_VIDEODEVICEFUNCS;', src, re.S)
members = re.findall(r'(PFND3D11_1DDI_\w+)\s+(pfn\w+);', m.group(1))
impl = set(sys.argv[2].split(','))
out = []
out.append('/* Generated from the WDK\'s d3d10umddi.h (D3D11_1DDI_VIDEODEVICEFUNCS) by\n * tools/gen-video-stubs.py; the entry points this driver does not carry out. */\n')
fill = []
for t, mem in members:
    name = mem[3:]
    mt = re.search(r'typedef\s+(\w+)\s*\(\s*APIENTRY\s*\*\s*' + t + r'\s*\)\s*\((.*?)\);', src, re.S)
    if not mt:
        raise SystemExit('no typedef for ' + t)
    ret, params = mt.group(1), ' '.join(mt.group(2).split())
    if name in impl:
        fill.append('   f->%s = Video%s;' % (mem, name))
        continue
    body = {'VOID': '', 'HRESULT': ' return E_NOTIMPL; ', 'SIZE_T': ' return 64; '}[ret]
    out.append('static %s APIENTRY\nVideoStub%s(%s)\n{%s}\n' % (ret, name, params, body))
    fill.append('   f->%s = VideoStub%s;' % (mem, name))
out.append('static void\nVideoFillFuncs(D3D11_1DDI_VIDEODEVICEFUNCS *f)\n{')
out += fill
out.append('}\n')
print('\n'.join(out))
