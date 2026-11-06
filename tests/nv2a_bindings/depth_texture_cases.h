/* Exercise conversion and actual texture binding across target rebinding. */
static void check_depth_view(Depth *expected)
{
#ifdef XBOXRECOMP_SDL3
    submit(NULL);
    CHECK(expected->y16 && bound[0].texture == expected->y16);
#else
    ID3D11ShaderResourceView *view = NULL;
    ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 1, &view);
    CHECK(expected->y16_srv && view == expected->y16_srv);
    if (view) ID3D11ShaderResourceView_Release(view);
#endif
}

static void test_depth_texture(void)
{
    NvD3DState st = draw_state(), probe = st;
    Depth *scene, *little, *large;
    uint32_t p = phys(st.zeta_addr);
    nv2a_d3d_clear(&st, 0xF3, 0xFF000000, 0x80000000u, 0, 0, 63, 63);
    scene = depth_find(p); CHECK(scene && scene->w == 64 && scene->h == 64);
    probe.color_addr = 0x60000; probe.color_pitch = 8 * 4;
    probe.clip_w = 8; probe.clip_h = 6;
    nv2a_d3d_clear(&probe, 0x02, 0, 0, 0, 0, 7, 5);
    little = depth_find(p); CHECK(little && little != scene && little->w == 8);
    st.tex_used = 1;
    st.tex[0].addr = st.zeta_addr; st.tex[0].color = 0x35;
    st.tex[0].width = 128; st.tex[0].height = 64;
    st.tex[0].pitch = 256; st.tex[0].levels = 1;
    st.tex[0].filter = (1u << 16) | (1u << 24); st.tex[0].address = 0x010101;
    triangle(&st, 0, 0);
    check_depth_view(scene);

    /* Both buffers cover the read; rebinding scene changes their recency. */
    probe.color_addr = 0x80000; probe.color_pitch = 128 * 4;
    probe.clip_w = probe.clip_h = 128;
    nv2a_d3d_clear(&probe, 0xF3, 0, 0x40000000u, 0, 0, 127, 127);
    large = depth_find(p); CHECK(large && large != scene && large->w == 128);
    triangle(&st, 0, 0);
    CHECK(depth_find(p) == scene);
    check_depth_view(large);
    puts("PASS: covering depth conversion and binding retain the same source across target rebinding");
}
