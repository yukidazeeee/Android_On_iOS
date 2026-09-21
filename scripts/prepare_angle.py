"""Write a deterministic standalone ANGLE gclient configuration at its locked commit."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def prepare(checkout):
    checkout = Path(checkout).resolve()
    dependency = next(item for item in json.loads((ROOT / 'ThirdParty/dependencies.lock.json').read_text())['references'] if item['name'] == 'angle')
    revision = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != dependency['commit']:
        raise ValueError('ANGLE checkout does not match dependencies.lock.json')
    content = ('solutions = ' + repr([{
        'name': '.', 'url': dependency['url'], 'managed': False,
        'custom_deps': {}, 'custom_vars': {
            'checkout_angle_internal': False,
            'checkout_angle_restricted_traces': False,
            'checkout_angle_cl_deps': False,
            'checkout_angle_dawn_deps': False,
        },
    }]) + '\ntarget_os = ["ios"]\n')
    config = checkout / '.gclient'
    if config.exists() and config.read_text() != content:
        raise ValueError('Refusing to replace a different ANGLE .gclient configuration')
    config.write_text(content)


if __name__ == '__main__':
    prepare(sys.argv[1])
