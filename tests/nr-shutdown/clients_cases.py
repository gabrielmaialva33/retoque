#!/usr/bin/env python3
"""Production NGX module membership: duplicate/nested shutdown and a module reloading."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'OptiScaler/inputs/NVNGX_DLSS.h').read_text()
source = source[source.index('class NVNGXClients'):source.index('void NVNGXDx11DeviceDestroyed')]
unit = r'''
#include <set>
#include <mutex>
#include <cassert>
using HMODULE=void*;
static HMODULE dllModule=(void*)1;
struct Util { static HMODULE GetCallerModule(void* caller){return caller;} };
#define LOG_INFO(...) ((void)0)
'''+source+r'''
int main(){
 NVNGXClients clients; auto game=(void*)2, sl=(void*)3, plugin=(void*)4;
 clients.Add(game); clients.Add(sl); clients.Add(plugin); clients.Add(game);
 assert(!clients.Remove(game)); assert(!clients.Remove(game));
 assert(!clients.Remove(sl)); assert(clients.Remove(plugin));
 assert(!clients.Remove(plugin)); // last client's second shutdown must not tear down again
 assert(!clients.Remove((void*)5)); // a module which never initialized owns no shutdown
 clients.Add(game); assert(clients.Remove(game)); // same address, new runtime generation
 clients.Add(sl); clients.Add(dllModule); assert(clients.Remove(dllModule)); // internal nested forwarding
 assert(clients.Remove(sl));
}
'''
with tempfile.TemporaryDirectory(prefix='retoque-ngx-clients-') as folder:
 p=Path(folder);(p/'cases.cpp').write_text(unit)
 subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'cases.cpp'),'-o',str(p/'cases')],check=True)
 subprocess.run([str(p/'cases')],check=True)
print('PASS: module membership, duplicate shutdown, nested internal forwarding, same-address reload')
