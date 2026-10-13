#!/usr/bin/env python3
"""Inspect real cross-target objects and exercise native path/source protections."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parent.parent
info = json.loads(subprocess.check_output([str(compiler), '--build-info'], text=True))
assert info['host_supported']
assert len(subprocess.check_output([str(compiler), '--list-targets'], text=True).splitlines()) == 4
expected_host = {('Darwin', 'arm64'): 'macos-arm64', ('Linux', 'x86_64'): 'linux-x86_64',
                 ('Linux', 'aarch64'): 'linux-arm64', ('Windows', 'AMD64'): 'windows-x86_64'}
assert info['host_target'] == expected_host[(platform.system(), platform.machine())]
tool_dir = Path(info['host_cc']).resolve().parent

def tool(name):
    for path in (tool_dir / name, tool_dir / (name + '.exe')):
        if path.is_file(): return str(path)
    found = shutil.which(name)
    if not found: raise RuntimeError('Required platform verification tool missing: ' + name)
    return found

clang, readobj = tool('clang'), tool('llvm-readobj')
report_dir = root / 'build' / 'platform-evidence' / info['host_target']
report_dir.mkdir(parents=True, exist_ok=True)
report = {'schema': 1, 'host': info, 'platform': platform.platform(), 'objects': [], 'path_checks': []}

def run(args, success=True, env=None):
    p = subprocess.run([str(a) for a in args], capture_output=True, encoding='utf-8', timeout=120, env=env)
    assert (p.returncode == 0) == success, (args, p.returncode, p.stdout, p.stderr)
    return p.stdout + p.stderr

with tempfile.TemporaryDirectory(prefix='bpp platform ') as temporary:
    work = Path(temporary) / 'project Ω with spaces'
    work.mkdir()
    source = work / 'main.bpp'
    helper = work / 'helper.bpp'
    helper.write_text('define proc answer(){return 42;}\n', encoding='utf-8')
    source.write_text('import "helper.bpp";\ndefine proc main(){return answer();}\n', encoding='utf-8')
    executable = work / ('program.exe' if os.name == 'nt' else 'program')
    temp = work / 'temporary Ω files'; temp.mkdir()
    env = dict(os.environ, TMP=str(temp), TEMP=str(temp), TMPDIR=str(temp))
    run([compiler, '--debug-info', '--print-result', '-o', executable, source], env=env)
    assert run([executable]).strip() == '42'
    assert Path(str(executable) + '.bpp-source').is_file()
    report['path_checks'].extend(['spaces', 'unicode', 'temporary directory', 'debug sidecar', 'native startup'])
    for target in (source, helper):
        before = target.read_bytes()
        for mode in ('--emit-ir', '--emit-asm', '--emit-dependencies', '--emit-map'):
            run([compiler, mode, '-o', target, source], success=False)
            assert target.read_bytes() == before
    report['path_checks'].append('root and imported source overwrite rejection')
    for path in (source, helper):
        alias = work / ('hardlink-' + path.name)
        os.link(path, alias)
        before = path.read_bytes()
        run([compiler, '--emit-asm', '-o', alias, source], success=False)
        assert path.read_bytes() == before
    report['path_checks'].append('hardlink source identity')
    snapshot = work / 'snapshot.bpp-source'
    snapshot.write_text(helper.read_text(), encoding='utf-8')
    source.write_text('import "snapshot.bpp-source";\ndefine proc main(){return answer();}\n', encoding='utf-8')
    before = snapshot.read_bytes()
    run([compiler, '--debug-info', '-o', work / 'snapshot', source], success=False)
    assert snapshot.read_bytes() == before
    report['path_checks'].append('imported debug-snapshot overwrite rejection')
    root_snapshot = work / 'root-output.bpp-source'
    root_snapshot.write_text('define proc main(){return 42;}\n', encoding='utf-8')
    before = root_snapshot.read_bytes()
    run([compiler, '--debug-info', '-o', work / 'root-output', root_snapshot], success=False)
    assert root_snapshot.read_bytes() == before
    report['path_checks'].append('root debug-snapshot overwrite rejection')
    if os.name == 'nt':
        source.write_text('define proc main(){return 42;}\n', encoding='utf-8')
        before = source.read_bytes()
        run([compiler, '--emit-asm', '-o', str(source).upper(), source], success=False)
        assert source.read_bytes() == before
        mixed = str(executable).replace('\\', '/')
        run([compiler, '--print-result', '-o', mixed, str(source)], env=env)
        assert run([executable]).strip() == '42'
        # Use native backslashes in manifest output and library search paths.
        library = work / 'libraries Ω'; library.mkdir()
        asm, obj = library / 'unit.s', library / 'unit.o'
        run([compiler, '--emit-library', '--library-name', 'paths', '--emit-asm', '-o', asm, helper])
        run([clang, '-c', '-x', 'assembler', asm, '-o', obj])
        run([tool('llvm-ar'), 'rcs', library / 'libpaths.bpp.a', obj])
        run([compiler, '--library-name', 'paths', '--emit-library-manifest', '-o', str(library / 'libpaths.bppabi'), helper])
        source.write_text('import library "paths";\ndefine proc main(){return answer();}\n', encoding='utf-8')
        run([compiler, '-L', str(library), '--print-result', '-o', executable, source], env=env)
        assert run([executable]).strip() == '42'
        report['path_checks'].extend(['drive letters', 'case-only alias', 'mixed separators', 'backslash static manifests'])
    # Force a large frame in baseline output. Exercise both ordinary value and
    # raw bootstrap backends; object readers inspect produced bytes, not text alone.
    source.write_text('define proc child(n){return n+1;}\ndefine proc main(){' +
                      ''.join('new byte v%d=%d;' % (i, i) for i in range(512)) +
                      'return child(' + '+'.join('v%d' % i for i in range(512)) + ');}\n', encoding='utf-8')
    targets = run([compiler, '--list-targets']).splitlines()
    for row in targets:
        target, triple, fmt, *_ = row.split('\t')
        descriptor = run([compiler, '--target-info', target])
        assert 'stack_alignment=16' in descriptor and 'helper_convention=' in descriptor
        (report_dir / (target + '.descriptor.txt')).write_text(descriptor, encoding='utf-8')
        for backend in ('values', 'raw'):
            stem = target + '-' + backend
            asm, obj = work / (stem + '.s'), work / (stem + '.o')
            options = ['--strict-native-runtime'] if backend == 'raw' else []
            run([compiler, '--debug-info', '--no-opt', *options, '--emit-asm', '--target', target, '-o', asm, source])
            run([clang, '-target', triple, '-g', '-c', '-x', 'assembler', asm, '-o', obj])
            dump = run([readobj, '--file-headers', '--sections', '--relocations', '--unwind', obj])
            text = asm.read_text(encoding='utf-8')
            if fmt == 'macho':
                assert '__text' in dump and '__debug_info' in dump and '__compact_unwind' in dump
                assert 'ARM64_RELOC_BRANCH26' in dump and 'ARM64_RELOC_PAGE21' in dump
            elif fmt == 'elf':
                assert '.text' in dump and '.debug_info' in dump and '.eh_frame' in dump and '.note.GNU-stack' in dump
                stack = dump.split('Name: .note.GNU-stack', 1)[1].split('Section {', 1)[0]
                assert 'SHF_EXECINSTR' not in stack
                assert ('R_AARCH64_CALL26' if 'arm64' in target else 'R_X86_64_PLT32') in dump
                assert '.cfi_startproc' in text
                assert ('str xzr, [sp]' if 'arm64' in target else 'movb $0, (%rsp)') in text
            else:
                assert '.text' in dump and '.pdata' in dump and '.xdata' in dump and '.debug$S' in dump
                assert 'IMAGE_REL_AMD64_REL32' in dump and 'UnwindInformation' in dump
                assert '___chkstk_ms' in dump and '.seh_stackalloc' in text
            (report_dir / (stem + '.readobj.txt')).write_text(dump, encoding='utf-8')
            shutil.copyfile(obj, report_dir / (stem + '.o'))
            shutil.copyfile(asm, report_dir / (stem + '.s'))
            report['objects'].append({'target': target, 'backend': backend, 'triple': triple,
                                      'sha256': hashlib.sha256(obj.read_bytes()).hexdigest()})
    if platform.system() == 'Darwin':
        report['sdk_version'] = run(['xcrun', '--show-sdk-version']).strip()
    report['tools'] = {name: run([path, '--version']).splitlines()[0] for name, path in [('clang', clang), ('llvm-readobj', readobj)]}
(report_dir / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print('Platform checks passed: native host/path/source identity; 8 real Mach-O/ELF/COFF objects, relocations, unwind, debug sections and large frames')
