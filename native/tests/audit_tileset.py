#!/usr/bin/env python3
"""只读审计显式 Tiles 树；统计拓扑与误差，不把节点数当作驻留/性能。"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
from urllib.parse import unquote, urlsplit


def audit(entry):
    fanout, depths, refine = Counter(), Counter(), Counter()
    manifests, contents, active = set(), set(), set()
    violations, empty_leaves = [], []
    nodes = 0
    def walk(tile, base, depth, parent_error=None):
        nonlocal nodes
        nodes += 1
        error = tile.get('geometricError', 0)
        if not isinstance(error, (float, int)) or not math.isfinite(error) or error < 0:
            violations.append({'kind': 'invalidError', 'base': str(base), 'error': error})
        elif parent_error is not None and error > parent_error:
            violations.append({'kind': 'childErrorExceedsParent', 'base': str(base), 'parent': parent_error, 'child': error})
        children = tile.get('children', [])
        fanout[len(children)] += 1
        depths[depth] += 1
        refine[tile.get('refine', 'inherited')] += 1
        references = tile.get('contents', [])
        if 'content' in tile:
            references = references + [tile['content']]
        if not references and not children and 'implicitTiling' not in tile:
            empty_leaves.append({'base': str(base), 'depth': depth})
        for reference in references:
            uri = reference.get('uri', reference.get('url', ''))
            parsed = urlsplit(uri)
            if parsed.scheme or parsed.netloc:
                raise ValueError('审计仅支持本地内容: ' + uri)
            path = (base / unquote(parsed.path)).resolve()
            if not path.is_file():
                raise FileNotFoundError(path)
            if path.suffix.lower() == '.json':
                load(path, depth + 1, error)
            else:
                contents.add(path)
        for child in children:
            walk(child, base, depth + 1, error)
    def load(path, depth, parent_error=None):
        if path in active:
            raise ValueError('清单引用循环: ' + str(path))
        active.add(path)
        manifests.add(path)
        doc = json.loads(path.read_text(encoding='utf-8'))
        walk(doc['root'], path.parent, depth, parent_error)
        active.remove(path)
    load(entry.resolve(), 0)
    return {'entry': str(entry.resolve()), 'nodes': nodes, 'manifests': len(manifests), 'uniqueContents': len(contents),
            'contentBytes': sum(p.stat().st_size for p in contents), 'maximumDepth': max(depths, default=0),
            'childrenCountHistogram': dict(sorted(fanout.items())), 'depthHistogram': dict(sorted(depths.items())),
            'refine': dict(refine), 'errorViolations': violations, 'emptyLeaves': empty_leaves,
            'scope': '静态显式树；不测运行时选层、共享纹理、内存、显存或帧耗时'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('entry', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = json.dumps(audit(args.entry), ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result, encoding='utf-8')
    else:
        print(result, end='')
