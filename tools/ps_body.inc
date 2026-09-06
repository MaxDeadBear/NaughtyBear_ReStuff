static void glsl_main() {
  auto& r = g_regs;
  for (int i=0;i<64;++i) r[i]=vec4(0.0);
  float ps = 0.0;
  bool p0 = false;
  int aL = 0;
  col_0 = vec4(0.0);
  r[0] = o_0;
  r[1] = o_1;
  r[2] = o_2;
  r[3] = o_3;
  r[4] = o_4;
  r[5] = o_5;
  r[6] = o_6;
  r[7] = o_7;
  r[8] = o_8;
  r[9] = o_9;
  if (pc.pgen.x > 0.5) {
    float px = gl_FragCoord.x - pc.pgen.z, py = gl_FragCoord.y - pc.pgen.z;
    r[int(pc.pgen.y + 0.5)] = vec4(gl_FrontFacing ? px : -px, py, 0.0, 0.0);
  }
  {
    vec4 smp = texture(tex_0, vec2(r[5].x, r[5].y));
    r[7].x = smp.x;
    r[7].y = smp.y;
    r[7].z = smp.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[3].zxyy()), vec4(r[3].zxyy())))); TRACEV(vr);
    r[5].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[0].zxyy()), vec4(r[0].zxyy())))); TRACEV(vr);
    r[4].y = vr.y;
  }
  {
    vec4 vr = ((-r[2].xyzz()) + c[4].xyzz()); TRACEV(vr);
    r[6].x = vr.x;
    r[6].y = vr.y;
    r[6].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[6].zxyy()), vec4(r[6].zxyy())))); TRACEV(vr);
    r[2].w = vr.w;
  }
  {
    vec4 vr = xmad(r[7].zyxx(), c[254].zzzz(), c[255].zzzz()); TRACEV(vr);
    r[9].x = vr.x;
    r[9].y = vr.y;
    r[9].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[9].xzyy()), vec4(r[9].xzyy())))); TRACEV(vr);
    ps = inversesqrt(abs(r[4].y)); TRACES(ps);
    r[0].w = vr.w;
    r[4].y = ps;
  }
  {
    vec4 vr = xmul(r[4].yyyy(), r[0].xxyz()); TRACEV(vr);
    ps = inversesqrt(abs(r[5].z)); TRACES(ps);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[0].xxxx(), r[3].zyxx()); TRACEV(vr);
    r[7].x = vr.x;
    r[7].y = vr.y;
    r[7].z = vr.z;
  }
  {
    vec4 vr = xmul(r[4].zyww(), r[7].zxyy()); TRACEV(vr);
    ps = inversesqrt(abs(r[2].w)); TRACES(ps);
    r[8].x = vr.x;
    r[8].y = vr.y;
    r[8].z = vr.z;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[6].zyxx(), r[0].xxxx()); TRACEV(vr);
    ps = inversesqrt(abs(r[0].w)); TRACES(ps);
    r[6].x = vr.x;
    r[6].y = vr.y;
    r[6].z = vr.z;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[9].zyxx(), r[0].xxxx()); TRACEV(vr);
    r[0].x = vr.x;
    r[0].y = vr.y;
    r[0].w = vr.w;
  }
  {
    vec4 vr = xmad(r[4].ywzz(), r[7].yzxx(), (-r[8].xyzz())); TRACEV(vr);
    r[8].x = vr.x;
    r[8].y = vr.y;
    r[8].z = vr.z;
  }
  {
    vec4 vr = xmul(r[8].xzyy(), r[3].wwww()); TRACEV(vr);
    r[3].x = vr.x;
    r[3].y = vr.y;
    r[3].z = vr.z;
  }
  {
    vec4 vr = xmul(r[0].yyyy(), r[3].yzxx()); TRACEV(vr);
    r[3].x = vr.x;
    r[3].y = vr.y;
    r[3].z = vr.z;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), r[7].zyxx(), r[3].xyzz()); TRACEV(vr);
    r[0].x = vr.x;
    r[0].y = vr.y;
    r[0].z = vr.z;
  }
  {
    vec4 vr = xmad(r[0].wwww(), r[4].wwzy(), r[0].zzyx()); TRACEV(vr);
    r[0].y = vr.y;
    r[0].z = vr.z;
    r[0].w = vr.w;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[6].xzyy()), vec4(r[0].ywzz())))); TRACEV(vr);
    ps = max(c[255].w, c[255].w); TRACES(ps);
    r[6].w = vr.w;
    r[0].x = ps;
  }
  {
    vec4 vr = (vec4(xdot4(vec4(r[6].xzyw()), vec4(r[0].ywzx())))); TRACEV(vr);
    r[2].w = vr.w;
  }
  {
    vec4 vr = xmad(r[2].wwww(), r[0].zwww(), (-r[6].yzzz())); TRACEV(vr);
    r[3].x = vr.x;
    r[3].y = vr.y;
  }
  {
    vec4 vr = xmad(r[3].xyyy(), c[255].xxxx(), c[255].xxxx()); TRACEV(vr);
    r[3].x = vr.x;
    r[3].y = vr.y;
  }
  {
    vec4 smp = texture(tex_1, vec2(r[3].y, r[3].x));
    r[3].x = smp.x;
    r[3].y = smp.y;
    r[3].z = smp.z;
  }
  {
    vec4 smp = texture(tex_2, vec2(r[5].x, r[5].y));
    r[4].y = smp.x;
    r[4].z = smp.y;
    r[4].w = smp.z;
  }
  {
    vec4 vr = ((-r[1].xyzz()) + c[255].wwww()); TRACEV(vr);
    r[1].x = vr.x;
    r[1].y = vr.y;
    r[1].z = vr.z;
  }
  {
    vec4 vr = xmul(r[1].xxxx(), r[4].wzyy()); TRACEV(vr);
    ps = max(c[8].z, c[8].z); TRACES(ps);
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
    r[4].w = ps;
  }
  {
    vec4 vr = xmul(r[5].xyzz(), r[1].yyyy()); TRACEV(vr);
    ps = max(c[8].y, c[8].y); TRACES(ps);
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
    r[4].z = ps;
  }
  {
    vec4 vr = xmul(r[5].zyxx(), r[1].zzzz()); TRACEV(vr);
    ps = max(c[8].x, c[8].x); TRACES(ps);
    r[1].x = vr.x;
    r[1].y = vr.y;
    r[1].z = vr.z;
    r[4].y = ps;
  }
  if (bcond(128) == true) {
  {
    vec4 vr = (vec4(xdot3(vec4(r[0].ywzz()), vec4(c[9].zxyy())))); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xxxx(), c[254].xxxx())); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), c[10].xxyz(), c[8].xxyz()); TRACEV(vr);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
  }
  if (bcond(129) == true) {
  {
    vec4 vr = (vec4(xdot3(vec4(r[0].ywzz()), vec4(c[11].zxyy())))); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xxxx(), c[254].xxxx())); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), c[12].xxyz(), r[4].yyzw()); TRACEV(vr);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
  }
  if (bcond(130) == true) {
  {
    vec4 vr = (vec4(xdot3(vec4(r[0].ywzz()), vec4(c[13].zxyy())))); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xxxx(), c[254].xxxx())); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), c[14].xxyz(), r[4].yyzw()); TRACEV(vr);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
  }
  if (bcond(131) == true) {
  {
    vec4 vr = (vec4(xdot3(vec4(r[0].ywzz()), vec4(c[15].zxyy())))); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xxxx(), c[254].xxxx())); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), c[16].xxyz(), r[4].yyzw()); TRACEV(vr);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
  }
  }
  }
  }
  }
  if (bcond(132) == true) {
  {
    vec4 vr = (r[2].xyzz() + (-c[17].xyzz())); TRACEV(vr);
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[5].zxyy()), vec4(r[5].zxyy())))); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw())); TRACEV(vr);
    ps = inversesqrt(abs(r[0].x)); TRACES(ps);
    r[2].w = ps;
  }
  {
    vec4 vr = ((-c[18].xxxx()) + c[255].wwww()); TRACEV(vr);
    ps = sqrt(abs(r[0].x)); TRACES(ps);
    r[1].w = vr.w;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[5].zyxx(), r[2].wwww()); TRACEV(vr);
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4((-r[5].xzyy())), vec4(r[0].ywzz())))); TRACEV(vr);
    ps = c[18].y - r[0].x; TRACES(ps);
    r[5].x = vr.x;
    r[5].y = ps;
  }
  {
    vec4 vr = (max(r[5].xxyy(), c[254].xxxx())); TRACEV(vr);
    ps = (1.0 / c[18].y); TRACES(ps);
    r[5].y = vr.y;
    r[5].z = vr.z;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[5].zzzz(), r[0].xxxx()); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), r[1].wwww(), c[18].xxxx()); TRACEV(vr);
    r[5].x = vr.x;
  }
  {
    vec4 vr = xmul(r[5].xxxx(), r[5].yxxx()); TRACEV(vr);
    r[6].x = vr.x;
    r[6].y = vr.y;
  }
  {
    vec4 vr = xmul(r[6].yxxx(), r[5].yxxx()); TRACEV(vr);
    r[5].x = vr.x;
    r[5].y = vr.y;
  }
  {
    vec4 vr = xmad(r[5].yyxx(), c[19].xxyz(), r[4].yyzw()); TRACEV(vr);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
  }
  if (bcond(133) == true) {
  {
    vec4 vr = (r[2].xyzz() + (-c[20].xyzz())); TRACEV(vr);
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[5].zxyy()), vec4(r[5].zxyy())))); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw())); TRACEV(vr);
    ps = inversesqrt(abs(r[0].x)); TRACES(ps);
    r[2].w = ps;
  }
  {
    vec4 vr = ((-c[21].xxxx()) + c[255].wwww()); TRACEV(vr);
    ps = sqrt(abs(r[0].x)); TRACES(ps);
    r[1].w = vr.w;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[5].zyxx(), r[2].wwww()); TRACEV(vr);
    r[5].x = vr.x;
    r[5].y = vr.y;
    r[5].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4((-r[5].xzyy())), vec4(r[0].ywzz())))); TRACEV(vr);
    ps = c[21].y - r[0].x; TRACES(ps);
    r[5].x = vr.x;
    r[5].y = ps;
  }
  {
    vec4 vr = (max(r[5].xxyy(), c[254].xxxx())); TRACEV(vr);
    ps = (1.0 / c[21].y); TRACES(ps);
    r[5].y = vr.y;
    r[5].z = vr.z;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[5].zzzz(), r[0].xxxx()); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), r[1].wwww(), c[21].xxxx()); TRACEV(vr);
    r[5].x = vr.x;
  }
  {
    vec4 vr = xmul(r[5].xxxx(), r[5].yxxx()); TRACEV(vr);
    r[6].x = vr.x;
    r[6].y = vr.y;
  }
  {
    vec4 vr = xmul(r[6].yxxx(), r[5].yxxx()); TRACEV(vr);
    r[5].x = vr.x;
    r[5].y = vr.y;
  }
  {
    vec4 vr = xmad(r[5].yyxx(), c[22].xxyz(), r[4].yyzw()); TRACEV(vr);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
  }
  if (bcond(134) == true) {
  {
    vec4 vr = (r[2].xxyz() + (-c[23].xxyz())); TRACEV(vr);
    r[2].y = vr.y;
    r[2].z = vr.z;
    r[2].w = vr.w;
  }
  {
    vec4 vr = (vec4(xdot3(vec4(r[2].wyzz()), vec4(r[2].wyzz())))); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw())); TRACEV(vr);
    ps = inversesqrt(abs(r[0].x)); TRACES(ps);
    r[2].x = ps;
  }
  {
    vec4 vr = ((-c[24].xxxx()) + c[255].wwww()); TRACEV(vr);
    ps = sqrt(abs(r[0].x)); TRACES(ps);
    r[1].w = vr.w;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[2].wzyy(), r[2].xxxx()); TRACEV(vr);
    r[2].x = vr.x;
    r[2].y = vr.y;
    r[2].z = vr.z;
  }
  {
    vec4 vr = (vec4(xdot3(vec4((-r[2].xzyy())), vec4(r[0].ywzz())))); TRACEV(vr);
    ps = c[24].y - r[0].x; TRACES(ps);
    r[0].y = vr.y;
    r[0].z = ps;
  }
  {
    vec4 vr = (max(r[0].yyzz(), c[254].xxxx())); TRACEV(vr);
    ps = (1.0 / c[24].y); TRACES(ps);
    r[0].y = vr.y;
    r[0].z = vr.z;
    r[0].x = ps;
  }
  {
    vec4 vr = xmul(r[0].zzzz(), r[0].xxxx()); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmad(r[0].xxxx(), r[1].wwww(), c[24].xxxx()); TRACEV(vr);
    r[0].x = vr.x;
  }
  {
    vec4 vr = xmul(r[0].xxxx(), r[0].yxxx()); TRACEV(vr);
    r[2].x = vr.x;
    r[2].y = vr.y;
  }
  {
    vec4 vr = xmul(r[2].yxxx(), r[0].yxxx()); TRACEV(vr);
    r[0].x = vr.x;
    r[0].y = vr.y;
  }
  {
    vec4 vr = xmad(r[0].yyxx(), c[25].xxyz(), r[4].yyzw()); TRACEV(vr);
    r[4].y = vr.y;
    r[4].z = vr.z;
    r[4].w = vr.w;
  }
  }
  }
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw())); TRACEV(vr);
    ps = max(r[4].x, r[4].x); TRACES(ps);
    r[0].y = ps;
  }
  {
    vec4 vr = xmul(c[6].wwww(), c[254].yyyy()); TRACEV(vr);
    ps = xmuls(c[33].x, r[0].y); TRACES(ps);
    r[2].x = vr.x;
    r[2].y = glm::clamp(ps, 0.0f, 1.0f);
  }
  {
    vec4 vr = ((-c[33].yyyy()) + c[255].wwww()); TRACEV(vr);
    ps = (-c[7].x) - (-r[0].y); TRACES(ps);
    r[0].z = vr.z;
    r[0].x = ps;
  }
  {
    vec4 vr = xmad(r[3].xyzz(), c[255].yyyy(), r[1].xyzz()); TRACEV(vr);
    r[1].x = vr.x;
    r[1].y = vr.y;
    r[1].z = vr.z;
  }
  {
    vec4 vr = xmad(r[1].xyzz(), r[4].yzww(), (-c[6].xyzz())); TRACEV(vr);
    r[1].x = vr.x;
    r[1].y = vr.y;
    r[1].z = vr.z;
  }
  {
    vec4 vr = xmul(r[2].yxxx(), r[0].zxxx()); TRACEV(vr);
    r[0].x = vr.x;
    r[0].y = vr.y;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw())); TRACEV(vr);
    ps = c[33].y + r[0].x; TRACES(ps);
    col_0.w = ps;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw())); TRACEV(vr);
    ps = exp2(r[0].y); TRACES(ps);
    r[0].x = ps;
  }
  {
    vec4 vr = (max(r[0].xyzw(), r[0].xyzw())); TRACEV(vr);
    ps = (1.0 / r[0].x); TRACES(ps);
    r[0].x = glm::clamp(ps, 0.0f, 1.0f);
  }
  {
    vec4 vr = xmad(r[1].xyzz(), r[0].xxxx(), c[6].xyzz()); TRACEV(vr);
    col_0.x = vr.x;
    col_0.y = vr.y;
    col_0.z = vr.z;
  }
  int af = int(pc.apc.y + 0.5);
  if (af != 7) {
    float aa = col_0.a, ar = pc.apc.x; bool ap = false;
    if (af==1) ap = aa <  ar; else if (af==2) ap = aa == ar;
    else if (af==3) ap = aa <= ar; else if (af==4) ap = aa >  ar;
    else if (af==5) ap = aa != ar; else if (af==6) ap = aa >= ar;
    if (!ap) { discarded = true; return; }
  }
}
