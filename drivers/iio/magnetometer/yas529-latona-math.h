/* SPDX-License-Identifier: GPL-2.0-or-later
 * Factory compensation from Yamaha (c)2010 yas_mag_kernel_driver.c.
 * Fixed source and provenance: docs/yas529-reference-provenance.json.
 */
#ifndef YAS529_LATONA_MATH_H
#define YAS529_LATONA_MATH_H
static inline void yas529_decode_factory(const unsigned char *cal, int matrix[9], int coeff[3])
{
 int i;
 matrix[0]=100;
 matrix[1]=(cal[0]>>2)-32;
 matrix[2]=((cal[0]&3)<<2)+(cal[1]>>6)-8;
 matrix[3]=(cal[1]&63)-32;
 matrix[4]=(cal[2]>>2)+38;
 matrix[5]=((cal[2]&3)<<4)+(cal[3]>>4)-32;
 matrix[6]=((cal[3]&15)<<2)+(cal[4]>>6)-32;
 matrix[7]=(cal[4]&63)-32;
 matrix[8]=(cal[5]>>1)+66;
 for(i=0;i<3;i++)coeff[i]=(int)cal[i+6]-128;
}
static inline void yas529_factory_xyz(const int raw[3], int temp,
 const int matrix[9], const int coeff[3], int xyz[3])
{
 long long centered[3],orthogonal[3],sum;
 int i,j;
 for(i=0;i<3;i++)
  centered[i]=(raw[i]-512)*16-((long long)(temp-256)*coeff[i]*0x7ae1)/65536;
 orthogonal[0]=-centered[0];
 orthogonal[1]=centered[2]-centered[1];
 orthogonal[2]=centered[2]+centered[1];
 for(i=0;i<3;i++){
  sum=0;for(j=0;j<3;j++)sum+=(long long)matrix[i*3+j]*orthogonal[j];
  xyz[i]=sum/1600;
 }
}
#endif
