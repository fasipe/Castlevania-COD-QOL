#!/usr/bin/env python3
# Setup for Linux and the Steam Deck (run by Setup-Linux.sh): what Setup.exe does on Windows,
# without Visual Studio, which does not install under Wine/Proton.
#
#   1. Downloads a toolchain into build-linux/ (once): LLVM's clang-cl and lld-link, which build
#      Windows programs natively on Linux, and, with xwin, Microsoft's C++ runtime and Windows SDK
#      headers and libraries (xwin asks you to accept Microsoft's license).
#   2. Takes your disc image and translates the game with build/codport.exe, run through Proton
#      (or the system's Wine).
#   3. Compiles the translated game into build/codport_game*.dll, as build-game.bat does.
# Then the game is build/codport.exe, added to Steam as a non-Steam game run with Proton.
#
# On Windows the same script builds with clang-cl instead of Visual Studio's compiler (CODPORT_LLVM:
# the LLVM folder; run from a Visual Studio developer prompt for its headers and libraries), which
# is how its compile step is tested.
import argparse
import concurrent.futures
import glob
import io
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, 'build')
TOOLS = os.path.join(ROOT, 'build-linux')
WINDOWS = os.name == 'nt'
LLVM_VERSION = '23.1.5'
LLVM_URL = f'https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/LLVM-{LLVM_VERSION}-Linux-X64.tar.xz'
XWIN_VERSION = '0.10.0'
XWIN_URL = (f'https://github.com/Jake-Shadle/xwin/releases/download/{XWIN_VERSION}/'
            f'xwin-{XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz')
PT = os.environ.get('LANG', '').lower().startswith('pt') or os.environ.get('LANGUAGE', '').lower().startswith('pt')


def say(en, pt=None):
    print(pt if PT and pt else en, flush=True)


def fail(en, pt=None):
    say('ERROR: ' + en, 'ERRO: ' + (pt or en))
    sys.exit(1)


# ---- the toolchain -----------------------------------------------------------------------------

def download(url, what):
    say(f'downloading {what}: {url}', f'baixando {what}: {url}')
    request = urllib.request.Request(url, headers={'User-Agent': 'codport-setup'})
    return urllib.request.urlopen(request)


class Progress(io.RawIOBase):
    """A download read through, printing how much has come."""
    def __init__(self, response):
        self.response, self.done, self.total, self.shown = response, 0, int(response.headers.get('Content-Length') or 0), 0

    def readable(self):
        return True

    def readinto(self, buffer):
        data = self.response.read(len(buffer))
        buffer[:len(data)] = data
        self.done += len(data)
        if self.done - self.shown >= 50 << 20 or not data:
            self.shown = self.done
            total = f' / {self.total >> 20} MB' if self.total else ''
            print(f'  {self.done >> 20} MB{total}', flush=True)
        return len(data)


def llvm_bin():
    if WINDOWS:
        base = os.environ.get('CODPORT_LLVM')
        if not base:
            fail('set CODPORT_LLVM to the LLVM folder (with bin\\clang-cl.exe)')
        return os.path.join(base, 'bin')
    return os.path.join(TOOLS, 'llvm', 'bin')


def ensure_llvm():
    """clang-cl and lld-link (and clang's own headers): a few hundred MB taken out of LLVM's release."""
    if WINDOWS or os.path.exists(os.path.join(llvm_bin(), 'lld-link')):
        return
    keep = ('bin/clang', 'bin/clang-cl', 'bin/lld', 'bin/lld-link')
    out = os.path.join(TOOLS, 'llvm')
    with tarfile.open(fileobj=io.BufferedReader(Progress(download(LLVM_URL, 'LLVM (about 2 GB, once)')), 1 << 20), mode='r|xz') as tar:
        for member in tar:
            name = member.name.split('/', 1)[1] if '/' in member.name else ''
            wanted = name in keep or name.startswith('bin/clang-') and name[10:].isdigit() or name.startswith('lib/clang/')
            if not wanted:
                continue
            member.name = name
            tar.extract(member, out, **({'filter': 'tar'} if hasattr(tarfile, 'tar_filter') else {}))
    if not os.path.exists(os.path.join(llvm_bin(), 'lld-link')):
        fail('LLVM was not unpacked', 'o LLVM nao foi extraido')


def sysroot():
    return os.path.join(TOOLS, 'xwin')


def ensure_sysroot():
    """Microsoft's C++ runtime and Windows SDK (headers and libraries), fetched from Microsoft by xwin."""
    if WINDOWS or os.path.isdir(os.path.join(sysroot(), 'crt', 'include')):
        return
    xwin = os.path.join(TOOLS, 'xwin-bin', 'xwin')
    if not os.path.exists(xwin):
        with tarfile.open(fileobj=io.BufferedReader(Progress(download(XWIN_URL, 'xwin')), 1 << 20), mode='r|gz') as tar:
            for member in tar:
                if member.name.endswith('/xwin') and member.isfile():
                    member.name = 'xwin'
                    tar.extract(member, os.path.dirname(xwin))
        os.chmod(xwin, 0o755)
    say('xwin now downloads the Microsoft C++ runtime and Windows SDK: it shows Microsoft\'s license and asks you to accept it.',
        'o xwin agora baixa o runtime de C++ e o SDK do Windows da Microsoft: ele mostra a licenca da Microsoft e pede que voce aceite.')
    cache = os.path.join(TOOLS, 'xwin-cache')
    r = subprocess.run([xwin, '--arch', 'x86_64', '--cache-dir', cache, 'splat', '--output', sysroot()])
    if r.returncode != 0 or not os.path.isdir(os.path.join(sysroot(), 'crt', 'include')):
        fail('xwin did not finish (the license must be accepted)', 'o xwin nao terminou (a licenca precisa ser aceita)')
    shutil.rmtree(cache, ignore_errors=True)  # the downloads, no longer needed


# ---- running codport.exe -----------------------------------------------------------------------

def steam_roots():
    home = os.path.expanduser('~')
    return [os.path.join(home, p) for p in ('.steam/steam', '.local/share/Steam', '.var/app/com.valvesoftware.Steam/data/Steam')]


def find_proton():
    found = []
    for root in steam_roots():
        found += glob.glob(os.path.join(root, 'steamapps', 'common', 'Proton*', 'proton'))
        found += glob.glob(os.path.join(root, 'compatibilitytools.d', '*', 'proton'))
    if not found:
        return None, None
    def rank(path):  # Experimental first, then the highest version number
        name = os.path.basename(os.path.dirname(path))
        digits = [int(''.join(c for c in part if c.isdigit()) or 0) for part in name.replace('-', ' ').replace('.', ' ').split()]
        return ('Experimental' in name, digits)
    proton = max(found, key=rank)
    steam = next(r for r in steam_roots() if proton.startswith(r))
    return proton, steam


def windows_path(path):
    return path if WINDOWS else 'Z:' + os.path.abspath(path).replace('/', '\\')


def runner():
    """The command that runs a Windows program here: itself on Windows, else CODPORT_WINE, the system's wine or Proton."""
    if WINDOWS:
        return [], dict(os.environ)
    if os.environ.get('CODPORT_WINE'):
        return [os.environ['CODPORT_WINE']], dict(os.environ)
    proton, steam = find_proton()
    if proton:
        say(f'running Windows programs with {proton}', f'rodando programas do Windows com {proton}')
        env = dict(os.environ, STEAM_COMPAT_DATA_PATH=os.path.join(TOOLS, 'proton-prefix'), STEAM_COMPAT_CLIENT_INSTALL_PATH=steam)
        os.makedirs(env['STEAM_COMPAT_DATA_PATH'], exist_ok=True)
        return [sys.executable, proton, 'run'], env
    if shutil.which('wine'):
        return ['wine'], dict(os.environ)
    fail('no Proton found: install one in Steam (e.g. run any Windows game once), or install wine',
         'nenhum Proton encontrado: instale um na Steam (por exemplo, rode um jogo de Windows uma vez) ou instale o wine')


def codport(args):
    prefix, env = runner()
    return subprocess.run(prefix + [os.path.join(BUILD, 'codport.exe')] + args, cwd=ROOT, env=env).returncode


# ---- the disc image ----------------------------------------------------------------------------

def is_game_iso(path):
    """An ISO 9660 image whose root directory names SLES_537.55 (as Setup's Test-Iso)."""
    try:
        with open(path, 'rb') as f:
            f.seek(16 * 2048)
            pvd = f.read(2048)
            if pvd[1:6] != b'CD001':
                return False
            lba = int.from_bytes(pvd[158:162], 'little')
            size = min(int.from_bytes(pvd[166:170], 'little'), 1 << 20)
            f.seek(lba * 2048)
            return b'SLES_537.55' in f.read(size)
    except OSError:
        return False


def ini_path():
    return os.path.join(ROOT, 'codport.ini')


def saved_iso():
    try:
        with open(ini_path(), encoding='utf-8-sig') as f:
            for line in f:
                key, _, value = line.partition('=')
                if key.strip() == 'iso' and value.strip():
                    v = value.strip()
                    return v[2:].replace('\\', '/') if v[:2].upper() == 'Z:' and not WINDOWS else v
    except OSError:
        pass
    return ''


def save_iso(path):
    """Into codport.ini ([disco] iso=...), as the game reads it: a Windows path (Z: is / under Wine)."""
    line = 'iso=' + windows_path(path)
    try:
        with open(ini_path(), encoding='utf-8-sig') as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    at = [i for i, l in enumerate(lines) if l.strip().startswith('iso') and l.partition('=')[0].strip() == 'iso']
    if at:
        lines[at[-1]] = line
    else:
        lines[0:0] = ['[disco]', line, '']
    with open(ini_path(), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')


def pick_iso():
    for command in (['zenity', '--file-selection', '--title=Castlevania: Curse of Darkness (SLES-53755) .iso', '--file-filter=*.iso *.ISO'],
                    ['kdialog', '--getopenfilename', os.path.expanduser('~'), '*.iso *.ISO']):
        if shutil.which(command[0]):
            r = subprocess.run(command, capture_output=True, text=True)
            return r.stdout.strip()
    return input('Path of the disc image (.iso) / Caminho da imagem do disco (.iso): ').strip().strip('"\'')


def find_iso(given):
    candidates = [given] if given else [saved_iso()]
    path = next((c for c in candidates if c and os.path.isfile(c)), '')
    if not path:
        path = pick_iso()
    if not path or not is_game_iso(path):
        fail(f'{path or "no file"} is not the European disc of Castlevania: Curse of Darkness (SLES-53755)',
             f'{path or "nenhum arquivo"} nao e o disco europeu de Castlevania: Curse of Darkness (SLES-53755)')
    return os.path.abspath(path)


# ---- compiling the game ------------------------------------------------------------------------

PARTS = [  # as CMakeLists.txt: the DLL, the folder of its sources; then build-game.bat's order
    ('codport_game', 'recomp/gen/boot'),
    ('codport_game_vu', 'recomp/vu'),
    ('codport_game_hot', 'recomp/gen/hot'),
    ('codport_game_cold', 'recomp/gen/cold'),
]


def jobs():
    n = os.cpu_count() or 4
    try:  # a translated file can take a couple of GB to compile: no more at once than memory holds
        with open('/proc/meminfo') as f:
            kb = next(int(l.split()[1]) for l in f if l.startswith('MemAvailable'))
        n = max(1, min(n, kb // (2 << 20)))
    except (OSError, StopIteration):
        pass
    return n


def tool(name):
    return os.path.join(llvm_bin(), name + ('.exe' if WINDOWS else ''))


def system_flags():
    if WINDOWS:
        return [], []  # INCLUDE and LIB from the developer prompt
    s = sysroot()
    includes = ['crt/include', 'sdk/include/ucrt', 'sdk/include/um', 'sdk/include/shared']
    libs = ['crt/lib/x86_64', 'sdk/lib/um/x86_64', 'sdk/lib/ucrt/x86_64']
    return ['/imsvc' + os.path.join(s, p) for p in includes], ['/libpath:' + os.path.join(s, p) for p in libs]


def compile_part(name, folder):
    sources = sorted(glob.glob(os.path.join(ROOT, folder, '*.cpp')))
    if not sources:
        fail(f'nothing to compile in {folder}', f'nada para compilar em {folder}')
    objdir = os.path.join(TOOLS, 'obj', name)
    os.makedirs(objdir, exist_ok=True)
    includes, libpaths = system_flags()
    flags = ['/nologo', '-w', '/DWIN32', '/D_WINDOWS', '/EHsc', '/O2', '/Ob2', '/DNDEBUG', '-std:c++20', '/MD', '/utf-8', '/permissive-',
             '/bigobj', '-msse4.1', '-D_CRT_SECURE_NO_WARNINGS', '-DCODPORT_GAME_DLL', f'-D{name}_EXPORTS', '/I' + os.path.join(ROOT, 'src')] + includes
    # -msse4.1: clang allows SSE4.1 intrinsics only where enabled; MSVC anywhere (the port needs SSE4.1 anyway).
    # Every generated file includes the same headers: parsed once (CMake's precompiled header).
    header, pch = os.path.join(objdir, 'pch.hxx'), os.path.join(objdir, 'pch.pch')
    with open(header, 'w') as f:
        f.write('#include "recomp/runtime.h"\n#include "ee/vu_ops.h"\n')
    with open(os.path.join(objdir, 'pch.cxx'), 'w') as f:
        f.write('#include "pch.hxx"\n')
    use = ['/Yu' + header, '/FI' + header, '/Fp' + pch]
    r = subprocess.run([tool('clang-cl')] + flags + ['/Yc' + header, '/FI' + header, '/Fp' + pch, '/c', os.path.join(objdir, 'pch.cxx'),
                                                     '/Fo' + os.path.join(objdir, 'pch.obj')])
    if r.returncode != 0:
        fail('the precompiled header did not compile', 'o cabecalho pre-compilado nao compilou')

    def one(source):
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(source))[0] + '.obj')
        if os.path.exists(obj) and os.path.getmtime(obj) > max(os.path.getmtime(source), os.path.getmtime(pch)):
            return obj, 0, b''  # done by an earlier run that stopped
        r = subprocess.run([tool('clang-cl')] + flags + use + ['/c', source, '/Fo' + obj], capture_output=True)
        return obj, r.returncode, r.stdout + r.stderr

    objects, done, start = [], 0, time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs()) as pool:
        for obj, code, output in pool.map(one, sources):
            done += 1
            if code != 0:
                sys.stdout.write(output.decode(errors='replace'))
                fail(f'{obj} did not compile', f'{obj} nao compilou')
            objects.append(obj)
            print(f'  {name}: {done}/{len(sources)} ({int(time.time() - start)} s)', flush=True)
    objects.append(os.path.join(objdir, 'pch.obj'))
    response = os.path.join(objdir, 'link.rsp')
    with open(response, 'w') as f:
        f.write('\n'.join(f'"{o}"' for o in objects))
    out = os.path.join(BUILD, name + '.dll')
    r = subprocess.run([tool('lld-link'), '/nologo', '/DLL', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/OUT:' + out,
                        '/IMPLIB:' + os.path.join(objdir, name + '.lib'), '@' + response, os.path.join(BUILD, 'codport.lib'),
                        'kernel32.lib', 'user32.lib'] + libpaths)
    if r.returncode != 0:
        fail(f'{name}.dll did not link', f'{name}.dll nao foi montado')


# ---- the steps ---------------------------------------------------------------------------------

def generated_stamp(iso):
    exe = os.path.join(BUILD, 'codport.exe')
    return f'{os.path.getsize(exe)} {int(os.path.getmtime(exe))} {iso}'


def main():
    parser = argparse.ArgumentParser(description='Builds the game on Linux / the Steam Deck (as Setup.exe on Windows).')
    parser.add_argument('iso', nargs='?', help='the disc image (default: the one saved in codport.ini, else asked for)')
    args = parser.parse_args()
    if not os.path.exists(os.path.join(BUILD, 'codport.exe')) or not os.path.exists(os.path.join(BUILD, 'codport.lib')):
        fail('build/codport.exe and build/codport.lib are missing: unpack the release zip whole',
             'faltam build/codport.exe e build/codport.lib: extraia o zip da release inteiro')
    os.makedirs(TOOLS, exist_ok=True)
    iso = find_iso(args.iso)
    save_iso(iso)
    say(f'disc image: {iso}', f'imagem do disco: {iso}')

    ensure_llvm()
    ensure_sysroot()

    stamp_file = os.path.join(TOOLS, 'generated.txt')
    stamp = generated_stamp(iso)
    if not os.path.exists(stamp_file) or open(stamp_file).read() != stamp:
        say('translating the game', 'traduzindo o jogo')
        for part, _ in PARTS[1:]:  # parts built from another translation: not for a game started now
            for ext in ('.ready', '.dll'):
                p = os.path.join(BUILD, part + ext)
                if os.path.exists(p):
                    os.remove(p)
        blocks = 'tools\\recomp\\vu-blocks.idx' + (';recomp\\vu-blocks.txt' if os.path.exists(os.path.join(ROOT, 'recomp', 'vu-blocks.txt')) else '')
        if codport([windows_path(iso), '--recompile', 'tools\\recomp\\functions.txt', 'recomp\\gen']) != 0 or \
                not os.path.exists(os.path.join(ROOT, 'recomp', 'gen', 'boot', 'table.cpp')):
            fail('the game was not translated', 'o jogo nao foi traduzido')
        if codport([windows_path(iso), '--vu-recompile', blocks, 'recomp\\vu']) != 0:
            fail('the VU programs were not translated', 'os programas do VU nao foram traduzidos')
        codport([windows_path(iso), '--make-icon', 'build\\castlevania.ico'])  # the window's icon (no harm if it fails)
        shutil.rmtree(os.path.join(TOOLS, 'obj'), ignore_errors=True)
        with open(stamp_file, 'w') as f:
            f.write(stamp)

    say(f'compiling, {jobs()} at a time (the slow part)', f'compilando, {jobs()} de cada vez (a parte demorada)')
    for i, (name, folder) in enumerate(PARTS):
        if i > 0 and os.path.exists(os.path.join(BUILD, name + '.ready')):
            continue
        compile_part(name, folder)
        if i > 0:
            open(os.path.join(BUILD, name + '.ready'), 'w').close()
        if i == 0:
            say('the game can already be played; the rest goes on (it makes it faster)',
                'o jogo ja pode ser jogado; o resto continua (deixa ele mais rapido)')
    say('done. In Steam: Add a Non-Steam Game -> build/codport.exe; Properties -> Compatibility -> force Proton.',
        'pronto. Na Steam: Adicionar jogo nao-Steam -> build/codport.exe; Propriedades -> Compatibilidade -> forcar o Proton.')


if __name__ == '__main__':
    main()
