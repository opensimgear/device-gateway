#!/usr/bin/env python3
"""Test isolated dependency snapshots using temporary local Git repositories."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
import bootstrap


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[2] / '.e2e'
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='bootstrap-test-', dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.repository('source')
        (self.source / 'tracked.txt').write_text('original\n')
        self.commit(self.source)
        self.runner = self.directory / 'runner'
        (self.runner / 'tests/e2e').mkdir(parents=True)
        (self.runner / 'tests/e2e/dependencies.json').write_text(json.dumps({
            'device-template': {'url': str(self.source), 'ref': 'main'},
        }))
        self.dependencies = bootstrap.Dependencies(self.runner, {'device-template': self.source})

    def git(self, repository, *arguments):
        return subprocess.check_output(['git', '-C', str(repository), *arguments], stderr=subprocess.PIPE).decode().strip()

    def repository(self, name):
        repository = self.directory / name
        repository.mkdir()
        self.git(repository, 'init', '--initial-branch=main')
        self.git(repository, 'config', 'commit.gpgsign', 'false')
        self.git(repository, 'config', 'user.name', 'Bootstrap test')
        self.git(repository, 'config', 'user.email', 'bootstrap-test@example.invalid')
        return repository

    def commit(self, repository):
        self.git(repository, 'add', '--all')
        self.git(repository, 'commit', '-m', 'Local test fixture')
        return self.git(repository, 'rev-parse', 'HEAD')

    def test_failed_initialization_and_update_are_retryable(self):
        original_command = bootstrap.command

        def fail_submodule(arguments, **kwargs):
            if 'submodule' in arguments:
                raise subprocess.CalledProcessError(1, list(map(str, arguments)))
            original_command(arguments, **kwargs)

        parent = self.runner / '.e2e/deps'
        with mock.patch.object(bootstrap, 'command', fail_submodule):
            with self.assertRaises(subprocess.CalledProcessError):
                self.dependencies.acquire('device-template')
        self.assertFalse((parent / 'device-template').exists())
        self.assertFalse((parent / 'device-template.source.json').exists())
        self.assertFalse(list(parent.glob('.device-template.prepare-*')))
        checkout = self.dependencies.acquire('device-template')
        self.assertEqual((checkout / 'tracked.txt').read_text(), 'original\n')
        (self.source / 'tracked.txt').write_text('changed\n')
        with mock.patch.object(bootstrap, 'command', fail_submodule):
            with self.assertRaises(subprocess.CalledProcessError):
                self.dependencies.acquire('device-template')
        self.assertEqual((checkout / 'tracked.txt').read_text(), 'original\n')
        checkout = self.dependencies.acquire('device-template')
        self.assertEqual((checkout / 'tracked.txt').read_text(), 'changed\n')
        self.assertEqual(json.loads((parent / 'device-template.source.json').read_text())['state'], 'ready')

    def test_interrupted_publication_receipt_allows_retry(self):
        checkout = self.dependencies.acquire('device-template')
        (self.source / 'tracked.txt').write_text('new snapshot\n')
        original_replace = os.replace

        def fail_publish(source, destination):
            if Path(destination) == checkout:
                raise OSError('Injected publication failure')
            return original_replace(source, destination)

        with mock.patch.object(bootstrap.os, 'replace', fail_publish):
            with self.assertRaisesRegex(OSError, 'publication failure'):
                self.dependencies.acquire('device-template')
        receipt = checkout.parent / 'device-template.source.json'
        self.assertEqual(json.loads(receipt.read_text())['state'], 'prepared')
        checkout = self.dependencies.acquire('device-template')
        self.assertEqual((checkout / 'tracked.txt').read_text(), 'new snapshot\n')
        self.assertEqual(json.loads(receipt.read_text())['state'], 'ready')

    def test_untracked_executable_mode_changes_refresh_snapshot(self):
        launcher = self.source / 'launcher.sh'
        launcher.write_text('#!/bin/sh\nexit 0\n')
        launcher.chmod(0o644)
        checkout = self.dependencies.acquire('device-template')
        self.assertEqual(stat.S_IMODE((checkout / launcher.name).stat().st_mode), 0o644)
        launcher.chmod(0o755)
        checkout = self.dependencies.acquire('device-template')
        self.assertEqual(stat.S_IMODE((checkout / launcher.name).stat().st_mode), 0o755)

    def test_changed_submodule_commit_is_initialized_and_dirty_contents_rejected(self):
        child = self.repository('child')
        (child / 'dependency.txt').write_text('first\n')
        first = self.commit(child)
        (child / 'dependency.txt').write_text('second\n')
        second = self.commit(child)
        self.git(self.source, '-c', 'protocol.file.allow=always', 'submodule', 'add', str(child), 'dependency')
        self.git(self.source / 'dependency', 'checkout', '--detach', first)
        self.commit(self.source)
        # Unstaged gitlink change is part of the requested working-tree snapshot.
        self.git(self.source / 'dependency', 'checkout', '--detach', second)
        with mock.patch.dict(os.environ, {'GIT_ALLOW_PROTOCOL': 'file'}):
            checkout = self.dependencies.acquire('device-template')
        self.assertEqual(self.git(checkout / 'dependency', 'rev-parse', 'HEAD'), second)
        self.assertEqual((checkout / 'dependency/dependency.txt').read_text(), 'second\n')
        (self.source / 'dependency/dependency.txt').write_text('uncommitted nested change\n')
        with self.assertRaisesRegex(ValueError, 'Dirty submodule contents'):
            self.dependencies.acquire('device-template')


if __name__ == '__main__':
    unittest.main(verbosity=2)
