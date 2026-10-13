"""Repository-local dependency provisioning for integration tests.

Only this repository's ignored .e2e directory is managed. A local --source is
cloned, then its tracked diff and non-ignored new files are copied explicitly so
cross-repository work can be tested before publication. Source trees are read-only.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def command(args, *, cwd=None, env=None):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args])


def source_arguments(values):
    result = {}
    for value in values:
        name, separator, directory = value.partition('=')
        if not separator or name not in {'cockpit', 'device-gateway', 'device-template', 'firmware-common'}:
            raise ValueError('--source must be REPOSITORY=/absolute/path')
        source = Path(directory).expanduser().resolve(strict=True)
        if not source.is_dir() or name in result:
            raise ValueError(f'Invalid or duplicate source: {value}')
        if Path(git(source, 'rev-parse', '--show-toplevel').decode().strip()).resolve() != source:
            raise ValueError(f'Source must be a Git repository root: {source}')
        result[name] = source
    return result


def atomic_json(path, value):
    """Publish a complete receipt; never leave a truncated JSON document."""
    descriptor, temporary = tempfile.mkstemp(prefix=f'.{path.name}.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write(json.dumps(value, indent=2) + '\n')
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def reject_dirty_submodules(source):
    """A clean changed gitlink can be copied; nested uncommitted files cannot."""
    for entry in git(source, 'ls-files', '--stage', '-z').split(b'\0'):
        if not entry or not entry.startswith(b'160000 '):
            continue
        name = entry.split(b'\t', 1)[1].decode()
        submodule = source / name
        if (submodule / '.git').exists() and git(submodule, 'status', '--porcelain', '--untracked-files=all').strip():
            raise ValueError(f'Dirty submodule contents cannot be snapshotted: {submodule}; commit or clean the nested changes first')


class Dependencies:
    def __init__(self, repository, sources):
        self.repository = Path(repository).resolve()
        self.directory = self.repository / '.e2e'
        if self.directory.is_symlink():
            raise ValueError('.e2e must not be a symlink')
        self.directory.mkdir(exist_ok=True)
        self.sources = sources
        self.resolved = {}
        self.manifest = json.loads((self.repository / 'tests/e2e/dependencies.json').read_text())

    def acquire(self, name, revision=None):
        specification = self.manifest.get(name, {'url': f'https://github.com/opensimgear/{name}.git', 'ref': 'main'})
        ref = revision or specification['ref']
        source = self.sources.get(name)
        fingerprint = {'url': specification['url'], 'ref': ref}
        patch = b''
        untracked = []
        if source:
            fingerprint['source'] = str(source)
            # firmware-common must match the owning firmware's declared pin.
            ref = revision or git(source, 'rev-parse', 'HEAD').decode().strip()
            fingerprint['sourceCommit'] = git(source, 'rev-parse', ref).decode().strip()
            if revision is None:
                reject_dirty_submodules(source)
                patch = git(source, 'diff', '--binary', 'HEAD')
                untracked = [item.decode() for item in git(source, 'ls-files', '--others', '--exclude-standard', '-z').split(b'\0') if item]
                digest = hashlib.sha256(patch)
                for item in sorted(untracked):
                    path = source / item
                    digest.update(item.encode() + b'\0')
                    digest.update(f'{path.lstat().st_mode:o}\0'.encode())
                    digest.update(os.readlink(path).encode() if path.is_symlink() else path.read_bytes())
                fingerprint['worktreeSha256'] = digest.hexdigest()
        parent = self.directory / 'deps'
        parent.mkdir(exist_ok=True)
        destination = parent / name
        receipt = parent / f'{name}.source.json'
        current = json.loads(receipt.read_text()) if receipt.is_file() else None
        # Remote symbolic refs are fetched again; an exact commit or local snapshot
        # can reuse its checkout when the fingerprint matches.
        reusable = source is not None or re.fullmatch(r'[0-9a-f]{40}', ref)
        if not (reusable and current and current.get('state', 'ready') == 'ready'
                and current['request'] == fingerprint and (destination / '.git').is_dir()):
            if (destination.exists() or destination.is_symlink()) and (not receipt.is_file() or destination.is_symlink()):
                raise ValueError(f'Refusing to replace an unmanaged checkout: {destination}')
            # Failed clone/checkout/patch/submodule attempts leave the previous
            # complete checkout usable and remove only their own temporary tree.
            with tempfile.TemporaryDirectory(prefix=f'.{name}.prepare-', dir=parent) as temporary:
                prepared = Path(temporary) / 'checkout'
                command(['git', 'clone', '--no-hardlinks', '--no-checkout', str(source or specification['url']), prepared])
                command(['git', '-C', prepared, 'checkout', '--detach', ref])
                if patch:
                    # Update gitlink entries in the index as well as ordinary
                    # files, so submodule initialization uses the source snapshot.
                    subprocess.run(['git', '-C', str(prepared), 'apply', '--index', '--binary', '-'], input=patch, check=True)
                for item in untracked:
                    original, target = source / item, prepared / item
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if original.is_symlink():
                        target.symlink_to(os.readlink(original))
                    else:
                        shutil.copy2(original, target)
                command(['git', '-C', prepared, 'submodule', 'update', '--init', '--recursive'])
                current = {'request': fingerprint, 'commit': git(prepared, 'rev-parse', 'HEAD').decode().strip(), 'state': 'prepared'}
                # Record ownership before publishing. If publication is interrupted,
                # this receipt permits a retry but cannot make a partial tree reusable.
                atomic_json(receipt, current)
                if destination.exists():
                    shutil.rmtree(destination)
                os.replace(prepared, destination)
                current['state'] = 'ready'
                atomic_json(receipt, current)
        self.resolved[name] = {**current, 'checkout': str(destination)}
        atomic_json(self.directory / 'resolved-dependencies.json', self.resolved)
        return destination

    def common(self, *firmware_repositories):
        revisions = set()
        for repository in firmware_repositories:
            text = (Path(repository) / 'firmware/cmake/common-revision.cmake').read_text()
            match = re.search(r'set\(OSG_COMMON_REVISION\s+([0-9a-f]{40})\)', text)
            if not match:
                raise ValueError(f'Missing firmware-common pin: {repository}')
            revisions.add(match.group(1))
        if len(revisions) != 1:
            raise ValueError('Firmware repositories require different firmware-common commits')
        return self.acquire('firmware-common', revisions.pop())
