extends SceneTree
## Read-only source bridge. Never calls save/load or writes Godot user data.
var out := "res://Engine/Cache/Bellwether/"
var mesh_ids := {}
var meshes: Array=[]
var draws: Array=[]
var colliders: Array=[]
var resources: Array=[]
var houses: Array=[]
var actors: Array=[]
var world: Node3D
func _initialize() -> void:
	call_deferred("export_world")
func v(p: Vector3) -> Array: return [p.x,p.y,p.z]
func matrix(t: Transform3D) -> Array: return v(t.basis.x)+[0]+v(t.basis.y)+[0]+v(t.basis.z)+[0]+v(t.origin)+[1]
func export_world() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(out+"meshes"))
	world=load("res://scenes/village.tscn").instantiate()
	world.saves_enabled=false;world.test_arena=true
	root.add_child(world)
	world.set_process(false);world.player.set_physics_process(false)
	world.beta.settings_enabled=false;world.story.checkpoints_enabled=false
	for e in world.enemies:e.set_physics_process(false)
	world.snow.set_process(false)
	for n in world.gatherables:
		resources.append({"id":str(n.name),"kind":n.kind,"position":v(n.global_position),"loot":n.loot()})
	for h in world.houses: houses.append({"id":str(h.name),"position":v(h.global_position)})
	for e in world.enemies: actors.append({"id":str(e.name),"kind":e.kind,"position":v(e.position),"health":e.health})
	for child in world.get_children():
		if child==world.snow or child==world.player or (child is CharacterBody3D and child in world.enemies) or child is CanvasLayer:continue
		walk(child,"","")
	FileAccess.open(out+"height.f32",FileAccess.WRITE).store_buffer(world.terrain.heights.to_byte_array())
	FileAccess.open(out+"ice.u8",FileAccess.WRITE).store_buffer(world.terrain.ice_bytes)
	FileAccess.open(out+"depth.u8",FileAccess.WRITE).store_buffer(world.snow.depth_bytes)
	var manifest={"format":1,"world_size":world.snow.WORLD_SIZE,"height_size":world.terrain.SIZE,"snow_size":world.snow.SIZE,"meshes":meshes,"draws":draws,"colliders":colliders,"resources":resources,"houses":houses,"actors":actors,"player":v(world.player.position)}
	FileAccess.open(out+"world.json",FileAccess.WRITE).store_string(JSON.stringify(manifest,"\t"))
	print("NATIVE_EXPORT meshes=",meshes.size()," draws=",draws.size()," colliders=",colliders.size()," resources=",resources.size()," actors=",actors.size())
	quit()
func walk(n: Node, owner_id: String, house: String) -> void:
	if n.is_in_group("gatherables"):owner_id=str(n.name)
	if n is Node3D and n in world.houses:house=str(n.name)
	if n is MeshInstance3D and n.mesh!=null and n.skin==null:
		for s in n.mesh.get_surface_count():
			var key:=str(n.mesh.get_instance_id())+"_"+str(s)
			if not mesh_ids.has(key):
				var a: Array=n.mesh.surface_get_arrays(s)
				if a.is_empty():continue
				var vertices: PackedVector3Array=a[Mesh.ARRAY_VERTEX]
				var normals: PackedVector3Array=a[Mesh.ARRAY_NORMAL]
				var uv: PackedVector2Array=a[Mesh.ARRAY_TEX_UV] if a[Mesh.ARRAY_TEX_UV]!=null else PackedVector2Array()
				var indices: PackedInt32Array=a[Mesh.ARRAY_INDEX]
				if indices.is_empty():
					for i in vertices.size():indices.append(i)
				var packed:=PackedFloat32Array()
				for i in vertices.size():
					var no:=normals[i] if normals.size()>i else Vector3.UP
					var u:=uv[i] if uv.size()>i else Vector2.ZERO
					packed.append_array([vertices[i].x,vertices[i].y,vertices[i].z,no.x,no.y,no.z,u.x,u.y])
				for i in range(0,indices.size(),3):
					var swap:=indices[i+1];indices[i+1]=indices[i+2];indices[i+2]=swap
				var id:=meshes.size();var path:="meshes/%05d.bin" % id
				var f:=FileAccess.open(out+path,FileAccess.WRITE)
				f.store_32(vertices.size());f.store_32(indices.size());f.store_buffer(packed.to_byte_array());f.store_buffer(indices.to_byte_array());f.close()
				meshes.append({"path":path,"vertices":vertices.size(),"indices":indices.size()});mesh_ids[key]=id
			var mat: Material=n.get_active_material(s)
			var col:=Color.WHITE;var rough:=.85;var metal:=0.0;var emission:=Color.BLACK
			if mat is BaseMaterial3D:
				col=mat.albedo_color;rough=mat.roughness;metal=mat.metallic
				if mat.emission_enabled:emission=mat.emission*mat.emission_energy_multiplier
			draws.append({"mesh":mesh_ids[key],"matrix":matrix(n.global_transform),"color":[col.r,col.g,col.b,col.a],"roughness":rough,"metallic":metal,"emission":[emission.r,emission.g,emission.b],"id":owner_id,"house":house,"part":str(n.name),"visible":n.is_visible_in_tree()})
	if n is CollisionShape3D and n.shape!=null and n.get_parent() is StaticBody3D and n.get_parent().collision_layer!=0:
		var c:={"matrix":matrix(n.global_transform),"id":owner_id,"house":house}
		if n.shape is BoxShape3D:c["type"]="box";c["size"]=v(n.shape.size)
		elif n.shape is CapsuleShape3D:c["type"]="capsule";c["radius"]=n.shape.radius;c["height"]=n.shape.height
		elif n.shape is CylinderShape3D:c["type"]="capsule";c["radius"]=n.shape.radius;c["height"]=n.shape.height
		else:c["type"]="unsupported"
		colliders.append(c)
	for child in n.get_children():walk(child,owner_id,house)
