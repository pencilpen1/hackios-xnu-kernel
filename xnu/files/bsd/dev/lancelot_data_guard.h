/* SPDX-License-Identifier: APSL-2.0
 * The Android FIEMAP receipt is bound to the boot payload by SHA-256. XNU
 * independently checks the entire initialized extent tree before publication,
 * and checks each requested sector against that immutable map before a write.
 * This header has no allocator, device writes or kernel-specific operations.
 */
#ifndef LANCELOT_DATA_GUARD_H
#define LANCELOT_DATA_GUARD_H
#define LHD_MAP_HEADER 32u
#define LHD_MAP_ENTRY 24u
#define LHD_MAX_RANGES 8192u
#define LHD_MAX_MAP_BYTES (LHD_MAP_HEADER + LHD_MAP_ENTRY * LHD_MAX_RANGES)
struct lhd_map { const uint8_t *raw; uint32_t count,block_size; uint64_t bytes; };
struct lhd_range { uint64_t logical,physical,count; };
struct lhd_walk { uint8_t node[6][4096]; unsigned index[6],entries[6]; uint64_t block[6]; };
static uint64_t lhd_le64(const uint8_t *p) { return e4_u32(p)|(uint64_t)e4_u32(p+4)<<32; }
static uint16_t lhd_be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]<<8|p[1]); }
static uint32_t lhd_be32(const uint8_t *p) { return (uint32_t)lhd_be16(p)<<16|lhd_be16(p+2); }
static uint64_t lhd_be64(const uint8_t *p) { return (uint64_t)lhd_be32(p)<<32|lhd_be32(p+4); }
static struct lhd_range lhd_range_at(const struct lhd_map *map,unsigned i) {
    const uint8_t *p=map->raw+LHD_MAP_HEADER+(size_t)i*LHD_MAP_ENTRY;
    return (struct lhd_range){lhd_le64(p),lhd_le64(p+8),lhd_le64(p+16)};
}
static int lhd_overlap(const struct lhd_map *map,uint64_t first,uint64_t count) {
    if(!count || first>UINT64_MAX-count)return count!=0;
    for(unsigned i=0;i<map->count;i++) {
        struct lhd_range r=lhd_range_at(map,i);
        if(first<r.physical+r.count && r.physical<first+count)return 1;
    }
    return 0;
}
static int lhd_map_open(struct lhd_map *out,const uint8_t *raw,size_t size,
    uint32_t block_size,uint64_t blocks,uint64_t file_bytes) {
    if(!out||!raw||size<LHD_MAP_HEADER||memcmp(raw,"LHDATA1\0",8)||e4_u32(raw+8)!=1 ||
       e4_u32(raw+12)!=block_size || lhd_le64(raw+16)!=file_bytes || e4_u32(raw+28) ||
       !block_size || file_bytes<UINT64_C(67108864)||(file_bytes&4095))return 0;
    unsigned count=e4_u32(raw+24);
    if(!count||count>LHD_MAX_RANGES||size!=LHD_MAP_HEADER+(size_t)count*LHD_MAP_ENTRY)return 0;
    struct lhd_map map={raw,count,block_size,file_bytes};uint64_t next=0;
    for(unsigned i=0;i<count;i++) {
        struct lhd_range r=lhd_range_at(&map,i);
        if(r.logical!=next||!r.count||!r.physical||r.physical>=blocks||r.count>blocks-r.physical ||
           r.count>file_bytes/block_size-next)return 0;
        for(unsigned j=0;j<i;j++) {struct lhd_range p=lhd_range_at(&map,j);
            if(r.physical<p.physical+p.count && p.physical<r.physical+r.count)return 0;}
        next+=r.count;
    }
    if(next!=file_bytes/block_size)return 0;
    *out=map;return 1;
}
static int lhd_map_block(const struct lhd_map *map,uint64_t logical,uint64_t *physical) {
    unsigned lo=0,hi=map->count;
    while(lo<hi) {unsigned mid=lo+(hi-lo)/2;struct lhd_range r=lhd_range_at(map,mid);
        if(logical<r.logical)hi=mid;
        else if(logical-r.logical>=r.count)lo=mid+1;
        else {*physical=r.physical+(logical-r.logical);return 1;}}
    return 0;
}
static int lhd_match_range(const struct lhd_map *map,uint64_t logical,uint64_t physical,uint64_t count) {
    while(count) {
        uint64_t expected;if(!lhd_map_block(map,logical,&expected)||expected!=physical)return 0;
        /* Ext4 leaf lengths are at most 32768 blocks. Check every block here;
         * this loop is linear over the file, without rereading the extent tree. */
        ++logical;++physical;--count;
    }
    return 1;
}
/* Visit each tree node once using caller-owned storage, rather than performing
 * millions of full tree walks or putting 24 KiB on the kernel stack. */
static int lhd_initialized_tree(struct ext4_fs *fs,const struct ext4_inode *inode,
    const struct lhd_map *map,struct lhd_walk *walk,uint64_t *nodes) {
    unsigned depth=e4_u16(inode->extents+6),level=0;uint64_t next=0;
    if(depth>5 || inode->size!=map->bytes || (inode->mode&0xf000)!=0x8000)return 0;
    memset(walk,0,sizeof(*walk));memcpy(walk->node[0],inode->extents,sizeof(inode->extents));
    for(;;) {
        uint8_t *node=walk->node[level];unsigned d=e4_u16(node+6);
        size_t size=level?fs->block_size:sizeof(inode->extents);
        unsigned n=e4_u16(node+2),max=e4_u16(node+4);
        if(e4_u16(node)!=0xf30a || d!=depth-level || !max || max>(size-12)/12 || !n || n>max)return 0;
        if(walk->index[level]==n) {
            if(!level)break;
            --level;continue;
        }
        unsigned i=walk->index[level]++;const uint8_t *e=node+12+12*i;uint32_t logical=e4_u32(e);
        if(i && logical<=e4_u32(e-12))return 0;
        if(d) {
            uint64_t child=e4_u32(e+4)|(uint64_t)e4_u16(e+8)<<32;
            if(!child||child>=fs->blocks||lhd_overlap(map,child,1)||logical!=next)return 0;
            for(unsigned j=0;j<=level;j++)if(walk->block[j]==child)return 0;
            ++level;walk->index[level]=0;walk->block[level]=child;
            if(!e4_extent_node(fs,child,walk->node[level]))return 0;
            if(nodes)++*nodes;
        } else {
            uint64_t physical=e4_u32(e+8)|(uint64_t)e4_u16(e+6)<<32;uint32_t count=e4_u16(e+4);
            if(logical!=next||!count||count>32768||!physical||physical>=fs->blocks ||
               count>fs->blocks-physical || next>map->bytes/fs->block_size ||
               count>map->bytes/fs->block_size-next || !lhd_match_range(map,next,physical,count))return 0;
            next+=count;
        }
    }
    return next==map->bytes/fs->block_size;
}
static int lhd_power_group(uint64_t group,unsigned base) {
    if(!group)return 0;while(group%base==0)group/=base;return group==1;
}
/* flex_bg may place a group's bitmaps/tables in another group. Protect the
 * actual descriptor locations, plus every primary/backup superblock and GDT. */
static int lhd_metadata_safe(struct ext4_fs *fs,const struct lhd_map *map,uint64_t *checked) {
    uint8_t sb[1024],desc[64];if(!e4_read(fs,1024,sizeof(sb),sb))return 0;
    uint32_t compat=e4_u32(sb+92),ro=e4_u32(sb+100);
    uint64_t groups=(fs->blocks-fs->first_block+fs->blocks_per_group-1)/fs->blocks_per_group;
    uint64_t tables=((uint64_t)fs->inodes_per_group*fs->inode_size+fs->block_size-1)/fs->block_size;
    uint64_t gdt=(groups*fs->desc_size+fs->block_size-1)/fs->block_size;
    for(uint64_t g=0;g<groups;g++) {
        if(!e4_read(fs,fs->descriptors+g*fs->desc_size,fs->desc_size>=64?64:32,desc))return 0;
        uint64_t bitmap=e4_u32(desc),ibitmap=e4_u32(desc+4),table=e4_u32(desc+8);
        if(fs->desc_size>=64){bitmap|=(uint64_t)e4_u32(desc+32)<<32;ibitmap|=(uint64_t)e4_u32(desc+36)<<32;table|=(uint64_t)e4_u32(desc+40)<<32;}
        if(!bitmap||!ibitmap||!table||bitmap>=fs->blocks||ibitmap>=fs->blocks||table>=fs->blocks ||
           tables>fs->blocks-table||lhd_overlap(map,bitmap,1)||lhd_overlap(map,ibitmap,1)||lhd_overlap(map,table,tables))return 0;
        int super=g==0 || ((compat&0x200)?(g==e4_u32(sb+588)||g==e4_u32(sb+592)):
            !(ro&1)||g==1||lhd_power_group(g,3)||lhd_power_group(g,5)||lhd_power_group(g,7));
        if(super) {
            uint64_t start=fs->first_block+g*fs->blocks_per_group,count=1+gdt+e4_u16(sb+206);
            if(start>=fs->blocks||count>fs->blocks-start||lhd_overlap(map,start,count))return 0;
        }
        if(checked)++*checked;
    }
    return 1;
}
static int lhd_hfs_geometry(const uint8_t *vh,const uint8_t *alternate,uint64_t bytes) {
    if(!vh||!alternate||vh[0]!='H'||vh[1]!='X'||lhd_be16(vh+2)!=5 ||
       memcmp(vh,alternate,4)||!(lhd_be32(vh+4)&(1u<<13)))return 0;
    uint32_t bs=lhd_be32(vh+40),total=lhd_be32(vh+44),free_blocks=lhd_be32(vh+48);
    if(bs<4096||bs>65536||(bs&(bs-1))||!total||(uint64_t)bs*total!=bytes ||
       free_blocks>total||lhd_be32(vh+52)>=total||!lhd_be32(vh+12)||lhd_be32(vh+12)>=total ||
       lhd_be32(alternate+40)!=bs||lhd_be32(alternate+44)!=total)return 0;
    for(unsigned fork=0;fork<5;fork++) {
        const uint8_t *p=vh+112+80*fork;uint64_t length=lhd_be64(p);uint32_t allocated=lhd_be32(p+12);
        if(allocated>total||length>(uint64_t)allocated*bs)return 0;
        if(fork<3 && (!length||!allocated))return 0;
        if(!fork && length<((uint64_t)total+7)/8)return 0;
        uint64_t inline_blocks=0;int ended=0;
        for(unsigned i=0;i<8;i++) {
            uint32_t first=lhd_be32(p+16+8*i),count=lhd_be32(p+20+8*i);
            if(!count){if(first)return 0;ended=1;continue;}
            if(ended||!first||first>=total||count>total-first)return 0;
            inline_blocks+=count;
        }
        if(inline_blocks>allocated || (!length&&allocated))return 0;
    }
    return 1;
}
#endif
