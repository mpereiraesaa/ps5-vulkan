// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PS5VK_TESS_POINT_MATRIX_H
#define PS5VK_TESS_POINT_MATRIX_H
/* Independent integer pixel oracle for equal/even level2 and odd level3.
 * Domain0=triangles,1=quads,2=isolines; spacing0=equal,1=even,2=odd.
 * Coordinates follow pinned CTS generateReference{Triangle,Quad,Isoline}TessCoords
 * in vktTessellationUtil.cpp. No observed GPU values enter this oracle.
 * Shader map is pixel centre=(4.5+54*u,4.5+54*v); every pixel is judged. */
static inline unsigned ps5vk_tess_point_count(unsigned domain,unsigned spacing)
{
    if(domain>2 || spacing>2)return 0;
    const unsigned n=spacing==2?3u:2u;
    return domain==0?(n==2?7u:12u):domain==1?(n+1)*(n+1):n*(n+1);
}
static inline int ps5vk_tess_point_inside(unsigned domain,unsigned spacing,
                                         unsigned x,unsigned y)
{
    if(domain>2 || spacing>2 || x<4 || y<4 || x>58 || y>58)return 0;
    const unsigned n=spacing==2?3u:2u,step=54u/n;
    x-=4;y-=4;
    if(domain==1)return x%step==0 && y%step==0;
    if(domain==2)return x%step==0 && y%step==0 && y<54;
    if((x==0 && y%step==0)||(y==0 && x%step==0)||
       (x+y==54 && x%step==0))return 1;
    if(n==2)return x==18 && y==18;
    return (x==30 && y==12)||(x==12 && y==30)||(x==12 && y==12);
}
#endif
