"""Build a small original fixture for the renderer without the game or Godot."""
import json
import pathlib
import struct
import sys

description = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))
output = pathlib.Path(sys.argv[2])
(output / 'meshes').mkdir(parents=True, exist_ok=True)
vertices, indices = [], []
faces = [
    ((0, 0, 1), [(-.5,0,.5),(.5,0,.5),(.5,1,.5),(-.5,1,.5)]),
    ((0, 0,-1), [(.5,0,-.5),(-.5,0,-.5),(-.5,1,-.5),(.5,1,-.5)]),
    ((1, 0, 0), [(.5,0,.5),(.5,0,-.5),(.5,1,-.5),(.5,1,.5)]),
    ((-1,0, 0), [(-.5,0,-.5),(-.5,0,.5),(-.5,1,.5),(-.5,1,-.5)]),
    ((0, 1, 0), [(-.5,1,.5),(.5,1,.5),(.5,1,-.5),(-.5,1,-.5)]),
    ((0,-1, 0), [(-.5,0,-.5),(.5,0,-.5),(.5,0,.5),(-.5,0,.5)])]
for normal, corners in faces:
    start = len(vertices) // 8
    for corner, uv in zip(corners, [(0,0),(1,0),(1,1),(0,1)]):
        vertices.extend((*corner, *normal, *uv))
    indices.extend(start + index for index in [0,1,2,0,2,3])
mesh_path = output / 'meshes/fixture.bin'
mesh_path.write_bytes(struct.pack('<II', len(vertices)//8, len(indices))
                      + struct.pack(f'<{len(vertices)}f', *vertices)
                      + struct.pack(f'<{len(indices)}I', *indices))
draws = []
for item in description['Objects']:
    x, y, z = item['Translation']
    sx, sy, sz = item['Scale']
    draws.append({'mesh':0, 'matrix':[sx,0,0,0,0,sy,0,0,0,0,sz,0,x,y,z,1],
                  'color':[*item['Color'],1], 'roughness':item['Roughness'],
                  'metallic':item['Metallic'], 'emission':item['Emission'], 'visible':True})
(output / 'height.f32').write_bytes(struct.pack('<16f', *([0]*16)))
(output / 'world.json').write_text(json.dumps({'format':1,'Fixture':'BellwetherRenderer_v1','height_size':4,'world_size':384,
    'meshes':[{'path':'meshes/fixture.bin'}], 'draws':draws, 'Renderer':description['Renderer']}), encoding='utf-8')
