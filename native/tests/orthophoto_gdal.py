"""Verify standalone orthophotos with GDAL, independently of the writer and report."""
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


cli, fixture, gdalinfo, gdallocationinfo = sys.argv[1:]


def geokeys(path):
    with path.open('rb') as stream:
        endian = {b'II': '<', b'MM': '>'}[stream.read(2)]
        version, directory = struct.unpack(endian + 'HI', stream.read(6))
        assert version == 42  # The small fixtures use classic TIFF.
        stream.seek(directory)
        count, = struct.unpack(endian + 'H', stream.read(2))
        for _ in range(count):
            tag, kind, length, offset = struct.unpack(endian + 'HHII', stream.read(12))
            if tag == 34735:
                assert kind == 3
                stream.seek(offset)
                return struct.unpack(endian + str(length) + 'H', stream.read(length * 2))
        return ()


with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    source = root / 'triangle.osgb'
    subprocess.run([fixture, '--write-triangle', source], check=True)
    cases = [
        ('EPSG:32651', '224107,3366639,42', 32651, (224107, 3366640)),
        ('ENU:0,123,0', '0,0,0', 32651, (500000, 1)),
        ('ENU:-33,151', '0,0,0', 32756, None),
        ('ENU:89,10', '0,0,0', 5041, None),
        ('LOCAL', '0,0,0', None, (0, 1)),
    ]
    for index, (srs, origin, epsg, control) in enumerate(cases):
        out = root / str(index)
        subprocess.run([cli, source, out, '--srs', srs, '--origin', origin], check=True, stdout=subprocess.DEVNULL)
        report = json.loads((out / 'conversion-report.json').read_text(encoding='utf-8'))['orthophoto']
        path = root / f'standalone-{index}.tif'
        shutil.copyfile(out / report['uri'], path)
        info = json.loads(subprocess.check_output([gdalinfo, '-json', path]))
        assert info['driverShortName'] == 'GTiff'
        assert info['size'] == [report['width'], report['height']]
        assert [b['colorInterpretation'] for b in info['bands']] == ['Red', 'Green', 'Blue', 'Alpha']
        west, xstep, xrot, north, yrot, ystep = info['geoTransform']
        assert (xstep, xrot, yrot, ystep) == (0.05, 0, 0, -0.05)
        assert math.dist(report['upperLeftEdge'], (west, north)) < 1e-7, (srs, west, north)
        keys = geokeys(path)
        if epsg:
            assert keys == (1, 1, 1, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, epsg)
            assert info['coordinateSystem']['wkt'].rstrip().endswith(f'ID["EPSG",{epsg}]]')
            assert info['metadata']['']['AREA_OR_POINT'] == 'Area'
            assert report['crs'] == f'EPSG:{epsg}'
        else:
            assert not keys and 'coordinateSystem' not in info and report['crs'] == 'LOCAL'
        if control:
            x, y = control
            assert math.hypot(west - x, north - y) < 0.001, (srs, west, north)
            for dx, dy, alpha in [(0.2, -0.8, 255), (0.8, -0.2, 0)]:
                actual = subprocess.check_output([gdallocationinfo, '-geoloc', '-valonly', '-b', '4', path,
                                                  str(x + dx), str(y + dy)])
                assert int(actual) == alpha, (srs, actual)
