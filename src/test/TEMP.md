 /* Save object crops */
        // if (frame_meta->num_obj_meta > 0) {
        //     for (NvDsMetaList *ol = frame_meta->obj_meta_list; ol; ol = ol->next) {
        //         obj_meta = (NvDsObjectMeta *) ol->data;

        //         NvDsObjEncUsrArgs args = { 0 };
        //         args.isFrame        = 0;          /* crop the object, not full frame */
        //         args.saveImg        = TRUE;
        //         args.attachUsrMeta  = FALSE;
        //         args.quality        = 80;

        //         /* file name: frame<N>_obj<id>_class<c>.jpg */
        //         snprintf (args.fileNameImg, sizeof (args.fileNameImg),
        //                   "%s/frame%04u_obj%lu_class%d.jpg",
        //                   OUTPUT_DIR,
        //                   g_frame_count,
        //                   (unsigned long) obj_meta->object_id,
        //                   obj_meta->class_id);

        //         /* Queue the encode request – non-blocking */
        //         if (nvds_obj_enc_process (g_enc_ctx, &args, surf, obj_meta, frame_meta)) {
        //             objects_queued = TRUE;
        //         } else {
        //             g_printerr ("[ERROR] nvds_obj_enc_process failed for obj %lu\n",
        //                         (unsigned long) obj_meta->object_id);
        //         }
        //     }
        //     g_print("[INFO] Queued full frame + %d object crops for frame %u\n",
        //         frame_meta->num_obj_meta, frame_meta->frame_num);
        // } else {
        //     g_print("[INFO] Queued full frame %u (no objects detected)\n",
        //         frame_meta->frame_num);
        // }