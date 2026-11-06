/* Shared CPU and real-GPU regression for 3D volume texture formats. */
static void test_volume(void)
{
    NvD3DTexture t = {0}; TexLayout l;
    uint32_t decoded[512], colors[4] = {0xFFFF0000u,0xFF00FF00u,0xFF0000FFu,0xFFFFFFFFu};
    uint16_t endpoints[4] = {0xF800,0x07E0,0x001F,0xFFFF};
    unsigned block, x, y, z, pass;
    NvD3DState st = state_for(16,2);
    NvD3DAttrib a[16] = {{0}};
    float positions[32][4] = {{0}}, coords[32][4] = {{0}}, defs[16][4] = {{0}};
    uint32_t indices[48];
    t.addr = 0x100000; t.format = 0x33310C39; t.color = 0x0C;
    t.width = t.height = 8; t.levels = 1; t.address = 0x030303;
    t.control0 = 0x40000000; t.filter = 0x01012000;
    CHECK(tex_volume(&t) && tex_layout(&t,&l));
    CHECK(l.level_d[0] == 8 && l.total == 256);
    /* Physical order: four consecutive z blocks, then the next x/y block. */
    for (block = 0; block < 32; ++block) {
        uint8_t *b = ram + t.addr + block * 8;
        uint16_t c = endpoints[(block % 4 + block / 16) % 4];
        memset(b,0,8); memcpy(b,&c,2);
    }
    decode_level(&t,&l,ram+t.addr,0,NULL,decoded);
    for (z=0;z<8;++z) for(y=0;y<8;++y) for(x=0;x<8;++x)
        CHECK(decoded[(z*8+y)*8+x] == colors[(z%4+z/4)%4]);
    for(z=0;z<8;++z) {
        unsigned k;
        for(k=0;k<4;++k) {
            positions[z*4+k][0] = (float)(2*z+2*(k&1));
            positions[z*4+k][1] = (float)(2*(k>>1));
            positions[z*4+k][2] = .5f; positions[z*4+k][3] = 1;
            coords[z*4+k][0] = coords[z*4+k][1] = .5f;
            coords[z*4+k][2] = (z+.5f)/8; coords[z*4+k][3] = 1;
        }
        indices[z*6]=z*4; indices[z*6+1]=z*4+1; indices[z*6+2]=z*4+2;
        indices[z*6+3]=z*4+2; indices[z*6+4]=z*4+1; indices[z*6+5]=z*4+3;
    }
    a[0].enabled=1;a[0].type=2;a[0].size=4;a[0].stride=16;a[0].data=(const uint8_t *)positions;
    a[9]=a[0];a[9].data=(const uint8_t *)coords;
    defs[10][0]=defs[11][1]=defs[12][2]=1;
    st.rc_seen=1; st.rc.final1=0x1800;
    for(pass=0;pass<3;++pass) {
        if (pass == 2) {
            t.address=0x040404;
            for(z=0;z<32;++z) coords[z][2] += 1;
        }
        st.rc.stage_program=pass == 1 ? 4u|(17u<<5)|(17u<<10)|(13u<<15) : 2u;
        st.rc.final0=pass == 1 ? 11u : 8u; st.rc.final1=pass == 1 ? 0x1B00 : 0x1800;
        st.tex[pass == 1 ? 3 : 0]=t;
        nv2a_d3d_clear(&st,0xF0,0xFF000000u,0,0,0,0xFFFF,0xFFFF);
        nv2a_d3d_draw_ub(&st,NV_D3D_TRIANGLES,a,defs,0,indices,48,0,31,NULL);
        for(z=0;z<8;++z) {
            uint32_t actual=volume_pixel(st.color_addr,2*z,0);
            if(actual != (pass == 2 ? 0u : colors[(z%4+z/4)%4])) fprintf(stderr,"volume pass %u z%u: %08X\n",pass,z,actual);
            CHECK(actual == (pass == 2 ? 0u : colors[(z%4+z/4)%4]));
        }
    }
    t.format=0x33310F39;t.color=0x0F;
    CHECK(tex_layout(&t,&l) && l.total==512);
    for(block=0;block<32;++block) {
        uint8_t *b=ram+t.addr+block*16;
        uint16_t c=endpoints[(block%4+block/16)%4];
        memset(b,0,16);b[0]=b[1]=255;memcpy(b+8,&c,2);
    }
    decode_level(&t,&l,ram+t.addr,0,NULL,decoded);
    for(z=0;z<8;++z) CHECK(decoded[z*64]==colors[(z%4+z/4)%4]);
    /* Rectangular Morton volume, plus mip offsets and a full-range hash span. */
    t.format=0x12330639;t.color=6;t.width=8;t.height=4;t.levels=3;
    CHECK(tex_layout(&t,&l) && l.level_d[0]==2 && l.level_d[1]==1);
    CHECK(l.level_off[1]==256 && l.level_off[2]==288 && l.total==296);
    for(block=0;block<64;++block) ((uint32_t *)(ram+t.addr))[block]=0xFF000000u|block;
    decode_level(&t,&l,ram+t.addr,0,NULL,decoded);
    CHECK(decoded[0]==0xFF000000u && decoded[32]==0xFF000004u && decoded[7]==0xFF000029u);
    puts("PASS: compressed 3D block order, every depth slice on GPU in PROJECT3D and DOT_STR_3D, W border addressing, BC3 alpha, Morton axes and mip spans");
}

/* Y16 is two byte lanes, not luminance: hardware returns (1, high, low, 1).
 * xemu ac72edfc2bc033cd1659123c44dc64f250332a60, verified against nxdk HW tests. */
static void test_y16(void)
{
    uint8_t packed[2]={0xEF,0xCD};
    NvD3DState source=state_for(8,2), st=state_for(2,2);
    NvD3DAttrib a[16]={{0}};
    float pos[4][4]={{0,0,.5f,1},{2,0,.5f,1},{0,2,.5f,1},{2,2,.5f,1}};
    float uv[4][2]={{0,.5f},{2,.5f},{0,.5f},{2,.5f}};
    float defs[16][4]={{0}}; uint32_t idx[6]={0,1,2,2,1,3};
    CHECK(texel(0x32,packed,NULL,0)==0xFFFFCDEFu);
    CHECK(texel(0x35,packed,NULL,0)==0xFFFFCDEFu);
    source.color_addr=0x50000;st.color_addr=0x60000;
    nv2a_d3d_clear(&source,0xF0,0x89ABCDEFu,0,0,0,0xFFFF,0xFFFF);
    st.rc_seen=1;st.rc.stage_program=1;st.rc.final0=8;st.rc.final1=0x1800;
    st.tex[0].addr=source.color_addr;st.tex[0].format=0x00013529;
    st.tex[0].color=0x35;st.tex[0].width=16;st.tex[0].height=2;st.tex[0].pitch=32;
    st.tex[0].levels=1;st.tex[0].address=0x030303;st.tex[0].filter=0x01012000;
    a[0].enabled=1;a[0].type=2;a[0].size=4;a[0].stride=16;a[0].data=(const uint8_t *)pos;
    a[9].enabled=1;a[9].type=2;a[9].size=2;a[9].stride=8;a[9].data=(const uint8_t *)uv;
    nv2a_d3d_clear(&st,0xF0,0,0,0,0,0xFFFF,0xFFFF);
    nv2a_d3d_draw_ub(&st,NV_D3D_TRIANGLES,a,defs,0,idx,6,0,3,NULL);
    CHECK(volume_pixel(st.color_addr,0,0)==0xFFFFCDEFu);
    CHECK(volume_pixel(st.color_addr,1,0)==0xFFFF89ABu);
    puts("PASS: Y16 byte lanes preserve one/high/low/one in CPU decode and GPU render-target aliases");
}
