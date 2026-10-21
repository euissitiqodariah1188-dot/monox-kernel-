import struct
strs=bytearray(); soff={}
def sname(n):
    if n not in soff: soff[n]=len(strs); strs.extend(n.encode()+b'\0')
    return soff[n]
st=bytearray()
def pad(): 
    while len(st)%4: st.append(0)
def begin(n): st.extend(struct.pack('>I',1)); st.extend(n.encode()+b'\0'); pad()
def end(): st.extend(struct.pack('>I',2))
def prop(n,v=b''):
    st.extend(struct.pack('>III',3,len(v),sname(n))); st.extend(v); pad()
def cells(*a): return b''.join(struct.pack('>I',x) for x in a)
def s(x): return x.encode()+b'\0'
begin('')
prop('#address-cells',cells(2)); prop('#size-cells',cells(2)); prop('model',s('TestBoard'))
begin('cpus'); prop('#address-cells',cells(1)); prop('#size-cells',cells(0))
begin('cpu@0'); prop('reg',cells(0)); end(); end()
begin('reserved-memory'); prop('#address-cells',cells(2)); prop('#size-cells',cells(2)); prop('ranges')
begin('fw@bff00000'); prop('reg',cells(0,0xbff00000,0,0x100000)); prop('no-map'); end(); end()
begin('memory@40000000'); prop('device_type',s('memory')); prop('reg',cells(0,0x40000000,0,0x80000000)); end()
begin('soc'); prop('#address-cells',cells(2)); prop('#size-cells',cells(2))
begin('interrupt-controller@f9000000'); prop('compatible',s('qcom,pdc')); prop('reg',cells(0,0xdead0000,0,0x1000)); end()
begin('intc@8000000'); prop('reg',cells(0,0x8000000,0,0x10000, 0,0x80a0000,0,0xf60000)); prop('compatible',s('arm,gic-v3')); prop('#address-cells',cells(2)); prop('#size-cells',cells(2)); prop('redistributor-stride',cells(0,0x20000))
begin('its@8080000'); prop('compatible',s('arm,gic-v3-its')); prop('reg',cells(0,0x8080000,0,0x20000)); end(); end()
begin('psci'); prop('compatible',s('arm,psci-1.0')); prop('method',s('hvc')); end()
begin('serial@9000000'); prop('compatible',s('arm,pl011')+s('arm,primecell')); prop('reg',cells(0,0x9000000,0,0x1000)); end()
begin('gpu@1000000'); prop('reg',cells(0,0x1000000,0,0x4000)); prop('interrupts',cells(0,40,4)); prop('compatible',s('arm,mali-bifrost')); end()
begin('i2c@a0000'); prop('#address-cells',cells(1)); prop('#size-cells',cells(0))
begin('touchscreen@14'); prop('compatible',s('goodix,gt9110')); prop('reg',cells(0x14)); prop('interrupts',cells(0,120,2)); prop('touchscreen-size-x',cells(1080)); prop('touchscreen-size-y',cells(2340)); end()
end(); end()
end()
st.extend(struct.pack('>I',9))
rsv=struct.pack('>QQ',0x47000000,0x200000)+struct.pack('>QQ',0,0)
off_rsv=40; off_struct=off_rsv+len(rsv); off_str=off_struct+len(st); total=off_str+len(strs)
hdr=struct.pack('>IIIIIIIIII',0xd00dfeed,total,off_struct,off_str,off_rsv,17,16,0,len(strs),len(st))
open('t.dtb','wb').write(hdr+rsv+bytes(st)+bytes(strs))
