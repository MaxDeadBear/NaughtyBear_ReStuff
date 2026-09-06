static void glsl_main() {
  vec4 r[64];
  for (int i=0;i<64;++i) r[i]=vec4(0.0);
  float ps = 0.0;
  bool p0 = false;
  int aL = 0;
  vec4 oPos = vec4(0.0,0.0,0.0,1.0);
  vec4 oPsz = vec4(1.0);
  o_0 = vec4(0.0);
  o_1 = vec4(0.0);
  o_2 = vec4(0.0);
  o_3 = vec4(0.0);
  o_4 = vec4(0.0);
  o_5 = vec4(0.0);
  o_6 = vec4(0.0);
  o_7 = vec4(0.0);
  o_8 = vec4(0.0);
  o_9 = vec4(0.0);
  o_10 = vec4(0.0);
  o_11 = vec4(0.0);
  o_12 = vec4(0.0);
  o_13 = vec4(0.0);
  o_14 = vec4(0.0);
  o_15 = vec4(0.0);
  r[1] = vec4(in_0.x, in_0.y, in_0.z, r[1].w);
  r[2] = vec4(in_1.y, in_1.z, r[2].z, in_1.x);
  r[6] = vec4(in_2.y, in_2.x, in_2.w, in_2.z);
  r[3] = vec4(in_3.y, in_3.x, r[3].z, r[3].w);
  r[3] = vec4(r[3].x, r[3].y, in_4.y, in_4.x);
  r[4] = vec4(in_5.z, in_5.y, in_5.x, in_5.w);
  {
    vec4 vr = ((r[1].zzzz() * c[2].xyzw() + c[3].xyzw()));
    r[0].x = vr.x;
    r[0].y = vr.y;
    r[0].z = vr.z;
    r[0].w = vr.w;
  }
  {
    vec4 vr = ((r[1].yyyy() * c[1].xzyw() + r[0].xzyw()));
    r[0].x = vr.x;
    r[0].y = vr.y;
    r[0].z = vr.z;
    r[0].w = vr.w;
  }
  {
    vec4 vr = ((r[1].xxxx() * c[0].wxyz() + r[0].wxzy()));
    r[0].x = vr.x;
    r[0].y = vr.y;
    r[0].z = vr.z;
    r[0].w = vr.w;
  }
  {
    vec4 vr = (max(r[0].yzwx(), r[0].yzwx()));
    oPos.x = vr.x;
    oPos.y = vr.y;
    oPos.z = vr.z;
    oPos.w = vr.w;
  }
  {
    vec4 vr = (r[2].yyyy() * c[6].xyzz());
    r[7].x = vr.x;
    r[7].y = vr.y;
    r[7].z = vr.z;
  }
  {
    vec4 vr = (r[6].zzzz() * c[6].xyzz());
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
  }
  {
    vec4 vr = ((r[1].zzzz() * c[6].xyzz() + c[7].xyzz()));
    r[8].x = vr.x;
    r[8].y = vr.y;
    r[8].z = vr.z;
  }
  {
    vec4 vr = ((r[1].yyyy() * c[5].xxyz() + r[8].xxyz()));
    r[0].y = vr.y;
    r[0].z = vr.z;
    r[0].w = vr.w;
  }
  {
    vec4 vr = ((r[2].xxxx() * c[5].xyzz() + r[7].xyzz()));
    r[2].x = vr.x;
    r[2].y = vr.y;
    r[2].z = vr.z;
  }
  {
    vec4 vr = ((r[6].yyyy() * c[5].xyzz() + r[5].xyzz()));
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
  }
  {
    vec4 vr = (r[0].xxxx() + (-c[8].xxxx()));
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw()));
    ps = max(r[6].w, r[6].w);
    o_3.w = ps;
  }
  {
    vec4 vr = ((r[6].xxxx() * c[4].xyzz() + r[5].xyzz()));
    o_3.x = vr.x;
    o_3.y = vr.y;
    o_3.z = vr.z;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw()));
    ps = c[8].y * r[0].x;
    o_4.x = ps;
  }
  {
    vec4 vr = (max(r[4].xyzw(), r[4].xyzw()));
    o_1.x = vr.x;
    o_1.y = vr.y;
    o_1.z = vr.z;
    o_1.w = vr.w;
  }
  {
    vec4 vr = (max(r[3].xyzw(), r[3].xyzw()));
    o_5.x = vr.x;
    o_5.y = vr.y;
    o_5.z = vr.z;
    o_5.w = vr.w;
  }
  {
    vec4 vr = ((r[2].wwww() * c[4].xyzz() + r[2].xyzz()));
    o_0.x = vr.x;
    o_0.y = vr.y;
    o_0.z = vr.z;
  }
  {
    vec4 vr = ((r[1].xxxx() * c[4].xyzz() + r[0].yzww()));
    o_2.x = vr.x;
    o_2.y = vr.y;
    o_2.z = vr.z;
  }
  vec2 _gp = oPos.xy() * pc.ndc.xy() + pc.ndc.zw() * oPos.w;
  gl_Position = vec4(_gp, oPos.z, oPos.w);
  gl_PointSize = oPsz.x;
}
