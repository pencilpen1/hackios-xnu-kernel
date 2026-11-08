} else { /* Inline data */ 
		if (exists) {
			result = remove_attribute_records(hfsmp, iterator);
			if (result) {
				goto exit;
			}
		}
		
		/* Calculate size of record rounded up to multiple of 2 bytes. */
		btdata.itemSize = sizeof(HFSPlusAttrData) - 2 + attrsize + ((attrsize & 1) ? 1 : 0);
		recp_size = btdata.itemSize;
		recp = hfs_malloc_data(recp_size);

		recp->recordType = kHFSPlusAttrInlineData;
		recp->attrData.reserved[0] = 0;
		recp->attrData.reserved[1] = 0;
		recp->attrData.attrSize = attrsize;
		
		/* Copy in the attribute data (if any). */
		if (attrsize > 0) {
			if (data_ptr) {
				bcopy(data_ptr, &recp->attrData.attrData, attrsize);
			} else {
				/* 
				 * A null UIO meant it originated in-kernel.  If they didn't supply data_ptr 
				 * then deny the copy operation.
				 */
				if (uio == NULL) {
					result = EPERM;
					goto exit;
				}
				result = uiomove((caddr_t)&recp->attrData.attrData, attrsize, uio);
			}
			
			if (result) {
				goto exit;
			}
		}
		
		(void) hfs_buildattrkey(target_id, ap->a_name, (HFSPlusAttrKey *)&iterator->key);
		
		btdata.bufferAddress = recp;
		btdata.itemCount = 1;
		result = BTInsertRecord(btfile, iterator, &btdata, btdata.itemSize);
	}
