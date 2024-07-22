/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Defines in this header are only used to define blend file storage.
 * This allows us to rename variables & structs without breaking compatibility.
 *
 * - When renaming the member of a struct which has itself been renamed
 *   refer to the newer name, not the original.
 *
 * - Changes here only change generated code for `makesdna.cc` and `makesrna.cc`
 *   without impacting Blender's run-time, besides allowing us to use the new names.
 *
 * - Renaming something that has already been renamed can be done
 *   by editing the existing rename macro.
 *   All references to the previous destination name can be removed since they're
 *   never written to disk.
 *
 * - Old names aren't sanity checked (since this file is the only place that knows about them)
 *   typos in the old names will break both backwards & forwards compatibility **TAKE CARE**.
 *
 * - Old names may be referenced as strings in versioning code which uses:
 *   #DNA_struct_exists & #DNA_struct_member_exists.
 *   The names used for versioning checks must be updated too.
 *
 * - Before editing rename defines run:
 *
 *   `sha1sum $BUILD_DIR/source/blender/makesdna/intern/dna.c`
 *
 *   Compare the results before & after to ensure all changes are reversed by renaming
 *   and the DNA remains unchanged.
 *
 * \see `versioning_dna.cc` for actual version patching.
 */

/* No include guard (intentional). */

/* Match RNA names where possible. */

/* NOTE: Keep sorted! */

DNA_STRUCT_RENAME(Lamp, Light)
DNA_STRUCT_RENAME(SeqRetimingHandle, SeqRetimingKey)
DNA_STRUCT_RENAME(SpaceButs, SpaceProperties)
DNA_STRUCT_RENAME(SpaceIpo, SpaceGraph)
DNA_STRUCT_RENAME(SpaceOops, SpaceOutliner)
DNA_STRUCT_RENAME_MEMBER(BPoint, alfa, tilt)
DNA_STRUCT_RENAME_MEMBER(BezTriple, alfa, tilt)
DNA_STRUCT_RENAME_MEMBER(Bone, curveInX, curve_in_x)
DNA_STRUCT_RENAME_MEMBER(Bone, curveInY, curve_in_z)
DNA_STRUCT_RENAME_MEMBER(Bone, curveOutX, curve_out_x)
DNA_STRUCT_RENAME_MEMBER(Bone, curveOutY, curve_out_z)
DNA_STRUCT_RENAME_MEMBER(Bone, scaleIn, scale_in_x)
DNA_STRUCT_RENAME_MEMBER(Bone, scaleOut, scale_out_x)
DNA_STRUCT_RENAME_MEMBER(Bone, scale_in_y, scale_in_z)
DNA_STRUCT_RENAME_MEMBER(Bone, scale_out_y, scale_out_z)
DNA_STRUCT_RENAME_MEMBER(BrushGpencilSettings, gradient_f, hardness)
DNA_STRUCT_RENAME_MEMBER(BrushGpencilSettings, gradient_s, aspect_ratio)
DNA_STRUCT_RENAME_MEMBER(Camera, YF_dofdist, dof_distance)
DNA_STRUCT_RENAME_MEMBER(Camera, clipend, clip_end)
DNA_STRUCT_RENAME_MEMBER(Camera, clipsta, clip_start)
DNA_STRUCT_RENAME_MEMBER(Collection, dupli_ofs, instance_offset)
DNA_STRUCT_RENAME_MEMBER(Curve, ext1, extrude)
DNA_STRUCT_RENAME_MEMBER(Curve, ext2, bevel_radius)
DNA_STRUCT_RENAME_MEMBER(Curve, len_wchar, len_char32)
DNA_STRUCT_RENAME_MEMBER(Curve, loc, texspace_location)
DNA_STRUCT_RENAME_MEMBER(Curve, size, texspace_size)
DNA_STRUCT_RENAME_MEMBER(Curve, texflag, texspace_flag)
DNA_STRUCT_RENAME_MEMBER(Curve, width, offset)
DNA_STRUCT_RENAME_MEMBER(Curves, attributes_active_index, attributes_active_index_legacy)
DNA_STRUCT_RENAME_MEMBER(CurvesGeometry, curve_size, curve_num)
DNA_STRUCT_RENAME_MEMBER(CurvesGeometry, point_size, point_num)
DNA_STRUCT_RENAME_MEMBER(CustomDataExternal, filename, filepath)
DNA_STRUCT_RENAME_MEMBER(Editing, over_border, overlay_frame_rect)
DNA_STRUCT_RENAME_MEMBER(Editing, over_cfra, overlay_frame_abs)
DNA_STRUCT_RENAME_MEMBER(Editing, over_flag, overlay_frame_flag)
DNA_STRUCT_RENAME_MEMBER(Editing, over_ofs, overlay_frame_ofs)
DNA_STRUCT_RENAME_MEMBER(FileAssetSelectParams, import_type, import_method)
DNA_STRUCT_RENAME_MEMBER(FileGlobal, filename, filepath)
DNA_STRUCT_RENAME_MEMBER(FluidDomainSettings, cache_frame_pause_guiding, cache_frame_pause_guide)
DNA_STRUCT_RENAME_MEMBER(FluidDomainSettings, guiding_alpha, guide_alpha)
DNA_STRUCT_RENAME_MEMBER(FluidDomainSettings, guiding_beta, guide_beta)
DNA_STRUCT_RENAME_MEMBER(FluidDomainSettings, guiding_parent, guide_parent)
DNA_STRUCT_RENAME_MEMBER(FluidDomainSettings, guiding_source, guide_source)
DNA_STRUCT_RENAME_MEMBER(FluidDomainSettings, guiding_vel_factor, guide_vel_factor)
DNA_STRUCT_RENAME_MEMBER(FluidEffectorSettings, guiding_mode, guide_mode)
DNA_STRUCT_RENAME_MEMBER(GreasePencil, drawing_array_size, drawing_array_num)
DNA_STRUCT_RENAME_MEMBER(GreasePencil, material_array_size, material_array_num)
DNA_STRUCT_RENAME_MEMBER(GreasePencilLayerFramesMapStorage, size, num)
DNA_STRUCT_RENAME_MEMBER(HookModifierData, totindex, indexar_num)
DNA_STRUCT_RENAME_MEMBER(Image, name, filepath)
DNA_STRUCT_RENAME_MEMBER(LaplacianDeformModifierData, total_verts, verts_num)
DNA_STRUCT_RENAME_MEMBER(Library, name, filepath)
DNA_STRUCT_RENAME_MEMBER(Light, energy, energy_deprecated)
DNA_STRUCT_RENAME_MEMBER(Light, energy_new, energy)
DNA_STRUCT_RENAME_MEMBER(LineartGpencilModifierData, line_types, edge_types)
DNA_STRUCT_RENAME_MEMBER(LineartGpencilModifierData, transparency_flags, mask_switches)
DNA_STRUCT_RENAME_MEMBER(LineartGpencilModifierData, transparency_mask, material_mask_bits)
DNA_STRUCT_RENAME_MEMBER(MDefCell, totinfluence, influences_num)
DNA_STRUCT_RENAME_MEMBER(MEdge, bweight, bweight_legacy)
DNA_STRUCT_RENAME_MEMBER(MEdge, crease, crease_legacy)
DNA_STRUCT_RENAME_MEMBER(MEdge, flag, flag_legacy)
DNA_STRUCT_RENAME_MEMBER(MPoly, flag, flag_legacy)
DNA_STRUCT_RENAME_MEMBER(MPoly, mat_nr, mat_nr_legacy)
DNA_STRUCT_RENAME_MEMBER(MVert, bweight, bweight_legacy)
DNA_STRUCT_RENAME_MEMBER(MVert, co, co_legacy)
DNA_STRUCT_RENAME_MEMBER(MVert, flag, flag_legacy)
DNA_STRUCT_RENAME_MEMBER(MaskLayer, restrictflag, visibility_flag)
DNA_STRUCT_RENAME_MEMBER(MaterialLineArt, transparency_mask, material_mask_bits)
DNA_STRUCT_RENAME_MEMBER(Mesh, edata, edge_data)
DNA_STRUCT_RENAME_MEMBER(Mesh, fdata, fdata_legacy)
DNA_STRUCT_RENAME_MEMBER(Mesh, ldata, corner_data)
DNA_STRUCT_RENAME_MEMBER(Mesh, loc, texspace_location)
DNA_STRUCT_RENAME_MEMBER(Mesh, pdata, face_data)
DNA_STRUCT_RENAME_MEMBER(Mesh, poly_offset_indices, face_offset_indices)
DNA_STRUCT_RENAME_MEMBER(Mesh, size, texspace_size)
DNA_STRUCT_RENAME_MEMBER(Mesh, smoothresh, smoothresh_legacy)
DNA_STRUCT_RENAME_MEMBER(Mesh, texflag, texspace_flag)
DNA_STRUCT_RENAME_MEMBER(Mesh, totedge, edges_num)
DNA_STRUCT_RENAME_MEMBER(Mesh, totface, totface_legacy)
DNA_STRUCT_RENAME_MEMBER(Mesh, totloop, corners_num)
DNA_STRUCT_RENAME_MEMBER(Mesh, totpoly, faces_num)
DNA_STRUCT_RENAME_MEMBER(Mesh, totvert, verts_num)
DNA_STRUCT_RENAME_MEMBER(Mesh, vdata, vert_data)
DNA_STRUCT_RENAME_MEMBER(MeshDeformModifierData, totcagevert, cage_verts_num)
DNA_STRUCT_RENAME_MEMBER(MeshDeformModifierData, totinfluence, influences_num)
DNA_STRUCT_RENAME_MEMBER(MeshDeformModifierData, totvert, verts_num)
DNA_STRUCT_RENAME_MEMBER(MetaBall, loc, texspace_location)
DNA_STRUCT_RENAME_MEMBER(MetaBall, size, texspace_size)
DNA_STRUCT_RENAME_MEMBER(MetaBall, texflag, texspace_flag)
DNA_STRUCT_RENAME_MEMBER(MovieClip, name, filepath)
DNA_STRUCT_RENAME_MEMBER(MovieTracking, act_plane_track, act_plane_track_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTracking, act_track, act_track_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTracking, plane_tracks, plane_tracks_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTracking, reconstruction, reconstruction_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTracking, tracks, tracks_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingCamera, principal, principal_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingSettings, keyframe1, keyframe1_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingSettings, keyframe2, keyframe2_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingStabilization, rot_track, rot_track_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingTrack, pat_max, pat_max_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingTrack, pat_min, pat_min_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingTrack, search_max, search_max_legacy)
DNA_STRUCT_RENAME_MEMBER(MovieTrackingTrack, search_min, search_min_legacy)
DNA_STRUCT_RENAME_MEMBER(NodeCryptomatte, num_inputs, inputs_num)
DNA_STRUCT_RENAME_MEMBER(NodeGeometryAttributeCapture, data_type, data_type_legacy)
DNA_STRUCT_RENAME_MEMBER(NodesModifierData, simulation_bake_directory, bake_directory)
DNA_STRUCT_RENAME_MEMBER(Object, col, color)
DNA_STRUCT_RENAME_MEMBER(Object, dup_group, instance_collection)
DNA_STRUCT_RENAME_MEMBER(Object, dupfacesca, instance_faces_scale)
DNA_STRUCT_RENAME_MEMBER(Object, restrictflag, visibility_flag)
DNA_STRUCT_RENAME_MEMBER(Object, size, scale)
DNA_STRUCT_RENAME_MEMBER(OpacityGpencilModifierData, hardeness, hardness)
DNA_STRUCT_RENAME_MEMBER(Paint, num_input_samples, num_input_samples_deprecated)
DNA_STRUCT_RENAME_MEMBER(ParticleSettings, child_nbr, child_percent)
DNA_STRUCT_RENAME_MEMBER(ParticleSettings, dup_group, instance_collection)
DNA_STRUCT_RENAME_MEMBER(ParticleSettings, dup_ob, instance_object)
DNA_STRUCT_RENAME_MEMBER(ParticleSettings, dupliweights, instance_weights)
DNA_STRUCT_RENAME_MEMBER(ParticleSettings, ren_child_nbr, child_render_percent)
DNA_STRUCT_RENAME_MEMBER(RenderData, bake_filter, bake_margin)
DNA_STRUCT_RENAME_MEMBER(RenderData, blurfac, motion_blur_shutter)
DNA_STRUCT_RENAME_MEMBER(RigidBodyWorld, steps_per_second, substeps_per_frame)
DNA_STRUCT_RENAME_MEMBER(SceneEEVEE, motion_blur_position, motion_blur_position_deprecated)
DNA_STRUCT_RENAME_MEMBER(SceneEEVEE, motion_blur_shutter, motion_blur_shutter_deprecated)
DNA_STRUCT_RENAME_MEMBER(SDefBind, numverts, verts_num)
DNA_STRUCT_RENAME_MEMBER(SDefVert, numbinds, binds_num)
DNA_STRUCT_RENAME_MEMBER(Sequence, retiming_handle_num, retiming_keys_num)
DNA_STRUCT_RENAME_MEMBER(Sequence, retiming_handles, retiming_keys)
DNA_STRUCT_RENAME_MEMBER(SpaceImage, pixel_snap_mode, pixel_round_mode)
DNA_STRUCT_RENAME_MEMBER(SpaceSeq, overlay_type, overlay_frame_type)
DNA_STRUCT_RENAME_MEMBER(Strip, dir, dirpath)
DNA_STRUCT_RENAME_MEMBER(StripElem, name, filename)
DNA_STRUCT_RENAME_MEMBER(StripProxy, dir, dirpath)
DNA_STRUCT_RENAME_MEMBER(StripProxy, file, filename)
DNA_STRUCT_RENAME_MEMBER(SurfaceDeformModifierData, num_mesh_verts, mesh_verts_num)
DNA_STRUCT_RENAME_MEMBER(SurfaceDeformModifierData, numpoly, target_polys_num)
DNA_STRUCT_RENAME_MEMBER(SurfaceDeformModifierData, numverts, bind_verts_num)
DNA_STRUCT_RENAME_MEMBER(Text, name, filepath)
DNA_STRUCT_RENAME_MEMBER(ThemeSpace, scrubbing_background, time_scrub_background)
DNA_STRUCT_RENAME_MEMBER(ThemeSpace, show_back_grad, background_type)
DNA_STRUCT_RENAME_MEMBER(UVProjectModifierData, num_projectors, projectors_num)
DNA_STRUCT_RENAME_MEMBER(UserDef, autokey_flag, keying_flag)
DNA_STRUCT_RENAME_MEMBER(UserDef, gp_manhattendist, gp_manhattandist)
DNA_STRUCT_RENAME_MEMBER(UserDef, pythondir, pythondir_legacy)
DNA_STRUCT_RENAME_MEMBER(VFont, name, filepath)
DNA_STRUCT_RENAME_MEMBER(View3D, far, clip_end)
DNA_STRUCT_RENAME_MEMBER(View3D, local_collections_uuid, local_collections_uid)
DNA_STRUCT_RENAME_MEMBER(View3D, local_view_uuid, local_view_uid)
DNA_STRUCT_RENAME_MEMBER(View3D, near, clip_start)
DNA_STRUCT_RENAME_MEMBER(View3D, ob_centre, ob_center)
DNA_STRUCT_RENAME_MEMBER(View3D, ob_centre_bone, ob_center_bone)
DNA_STRUCT_RENAME_MEMBER(View3D, ob_centre_cursor, ob_center_cursor)
DNA_STRUCT_RENAME_MEMBER(bArmature, collections, collections_legacy)
DNA_STRUCT_RENAME_MEMBER(bGPDstroke, gradient_f, hardness)
DNA_STRUCT_RENAME_MEMBER(bGPDstroke, gradient_s, aspect_ratio)
DNA_STRUCT_RENAME_MEMBER(bNodeLink, multi_input_socket_index, multi_input_sort_id)
DNA_STRUCT_RENAME_MEMBER(bNodeTree, inputs, inputs_legacy)
DNA_STRUCT_RENAME_MEMBER(bNodeTree, outputs, outputs_legacy)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, curveInX, curve_in_x)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, curveInY, curve_in_z)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, curveOutX, curve_out_x)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, curveOutY, curve_out_z)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, scaleIn, scale_in_x)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, scaleOut, scale_out_x)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, scale_in_y, scale_in_z)
DNA_STRUCT_RENAME_MEMBER(bPoseChannel, scale_out_y, scale_out_z)
DNA_STRUCT_RENAME_MEMBER(bSameVolumeConstraint, flag, free_axis)
DNA_STRUCT_RENAME_MEMBER(bSound, name, filepath)
DNA_STRUCT_RENAME_MEMBER(bTheme, tact, space_action)
DNA_STRUCT_RENAME_MEMBER(bTheme, tbuts, space_properties)
DNA_STRUCT_RENAME_MEMBER(bTheme, tclip, space_clip)
DNA_STRUCT_RENAME_MEMBER(bTheme, tconsole, space_console)
DNA_STRUCT_RENAME_MEMBER(bTheme, text, space_text)
DNA_STRUCT_RENAME_MEMBER(bTheme, tfile, space_file)
DNA_STRUCT_RENAME_MEMBER(bTheme, tima, space_image)
DNA_STRUCT_RENAME_MEMBER(bTheme, tinfo, space_info)
DNA_STRUCT_RENAME_MEMBER(bTheme, tipo, space_graph)
DNA_STRUCT_RENAME_MEMBER(bTheme, tnla, space_nla)
DNA_STRUCT_RENAME_MEMBER(bTheme, tnode, space_node)
DNA_STRUCT_RENAME_MEMBER(bTheme, toops, space_outliner)
DNA_STRUCT_RENAME_MEMBER(bTheme, tseq, space_sequencer)
DNA_STRUCT_RENAME_MEMBER(bTheme, tstatusbar, space_statusbar)
DNA_STRUCT_RENAME_MEMBER(bTheme, ttopbar, space_topbar)
DNA_STRUCT_RENAME_MEMBER(bTheme, tuserpref, space_preferences)
DNA_STRUCT_RENAME_MEMBER(bTheme, tv3d, space_view3d)
DNA_STRUCT_RENAME_MEMBER(bUserAssetLibrary, path, dirpath)
/* NOTE: Keep sorted! */

/* Write with a different name, old Blender versions crash loading files with non-NULL
 * global_areas. See D9442. */
DNA_STRUCT_RENAME_MEMBER(wmWindow, global_area_map, global_areas)

/* NOTE: Keep sorted! */
