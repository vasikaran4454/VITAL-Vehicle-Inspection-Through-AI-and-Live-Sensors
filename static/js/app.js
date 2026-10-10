const $=id=>document.getElementById(id),$$=s=>document.querySelectorAll(s);
const S={live:false,mode:'idle',ip:localStorage.getItem('vital_ip')||'',d:null,al:[],n:0,t0:Date.now(),pkT:0,pkI:0,poll:null,fails:0,file:null,img:null,ins:[],h:{c1:[],c2:[],c3:[],t1:[],t2:[],t3:[],cur:[],vib:[],noise:[],pack:[],l:[]}};
const OKS=['NORMAL','OK','GOOD'],REDS=['ALERT','FAIL','POOR'];
const col=s=>!s||s==='ERR'?'var(--t3)':OKS.includes(s)?'var(--ac)':REDS.includes(s)?'var(--rd)':'var(--am)';
const num=(v,d=1)=>v==null?'--':(+v).toFixed(d);
const st=(id,s)=>{const e=$(id);e.textContent=s||'--';e.style.color=col(s)};
const card=(ic,c,t,b)=>`<div class="card sc"><div class="sh"><div class="ic" style="background:${c}1a"><i class="fas ${ic}" style="color:${c}"></i></div><span class="lbl" style="margin:0;text-align:right">${t}</span></div>${b}</div>`;
const trio=(p,u,c)=>`<div style="display:flex;gap:6px;margin-top:6px">`+[1,2,3].map(i=>`<div style="flex:1;text-align:center;background:var(--bg);border-radius:8px;padding:8px 2px"><div class="fd" id="${p}${i}" style="font-size:22px;font-weight:700;color:var(--t3)">--</div><small style="color:var(--t3);font-size:10px">${u}${i}</small></div>`).join('')+`</div>`;
const sf=(a,b)=>`<div class="sf"><span>${a}</span><span id="${b}">--</span></div>`;
const CARDS=[
 card('fa-car-battery','#60a5fa','Battery Pack (3S)',`<div class="sv" id="v-pack" style="color:#60a5fa">--</div><div class="sf"><span>V pack</span><span id="tr-imb">--</span></div>${trio('c','C')}`),
 card('fa-temperature-three-quarters','#ff6432','Temperature (DS18B20)',`${trio('t','T')}<div class="sf"><span>°C</span><span>Battery / Coolant / Port</span></div>`),
 card('fa-bolt','#facc15','Current / Power (INA219)',`<div class="sv" id="v-cur" style="color:#facc15">--</div>${sf('mA','s-cur')}<div class="sf"><span id="v-pw">-- mW</span><span id="v-bus">-- V bus</span></div>`),
 card('fa-wave-square','#2dd4bf','Vibration / Tilt (ADXL345)',`<div class="sv" id="v-vib" style="color:#2dd4bf">--</div>${sf('m/s² vibration','s-vib')}<div class="sf"><span id="v-tilt">tilt --</span><span id="s-tilt">--</span></div><div class="sf"><span id="v-rp">roll -- pitch --</span><span></span></div>`),
 card('fa-microphone','#c084fc','Noise (INMP441)',`<div class="sv" id="v-noise" style="color:#c084fc">--</div>${sf('dB','s-noise')}`),
 card('fa-droplet','#f59e0b','Oil Purity',`<div class="sv" id="v-oil" style="color:#f59e0b">--</div>${sf('% purity','s-oil')}<div class="bar"><i id="b-oil" style="background:#f59e0b"></i></div>`),
 card('fa-magnet','#ff6432','Metal Debris',`<div class="sv" id="v-metal" style="font-size:24px;color:var(--t3)">--</div><div class="sf"><span>Wear-debris sensor</span><span></span></div>`),
 card('fa-shield-halved','#00e68a','Insulation (IMD)',`<div class="sv" id="v-ins" style="font-size:24px;color:var(--t3)">--</div><div class="sf"><span>Insulation test</span><span></span></div>`)
].join('');
$('cards').innerHTML=CARDS;

// connection UI (rendered in sidebar + mobile tab)
$$('.conn').forEach(e=>e.innerHTML=`<input class="in ip" placeholder="192.168.1.100" inputmode="decimal" spellcheck="false" autocomplete="off" value="${S.ip}"><button class="btn bc"><i class="fas fa-plug"></i> Connect</button><div class="stw s-idle"><span class="dot"></span><span class="stt">Not Connected</span></div><div class="err hide"></div>`);
$$('.ip').forEach(i=>{i.oninput=()=>$$('.ip').forEach(o=>o.value=i.value);i.onkeydown=e=>{if(e.key==='Enter')toggle()}});
$$('.bc').forEach(b=>b.onclick=toggle);

function toast(m,t='info'){const e=document.createElement('div');e.className='toast '+t;e.textContent=m;$('toasts').appendChild(e);setTimeout(()=>{e.style.opacity=0;setTimeout(()=>e.remove(),300)},3200)}
function status(mode,txt,err){
 S.mode=mode;
 $$('.stw').forEach(e=>e.className='stw s-'+mode);
 $$('.stt').forEach(e=>e.textContent=txt);
 $$('.pill').forEach(e=>e.style.color=mode==='error'?'var(--rd)':mode==='connecting'?'var(--am)':mode==='idle'?'var(--t3)':'var(--ac)');
 $$('.err').forEach(e=>{e.classList.toggle('hide',!err);e.textContent=err||''});
}
function btn(html,cls,dis){$$('.bc').forEach(b=>{b.innerHTML=html;b.className='btn bc'+(cls?' '+cls:'');b.disabled=!!dis})}
async function toggle(){
 if(S.live)return disc();
 const ip=$$('.ip')[0].value.trim().replace(/^https?:\/\//,'').replace(/\/.*$/,'');
 if(!ip)return toast('ESP32 IP type pannunga','error');
 S.ip=ip;localStorage.setItem('vital_ip',ip);
 status('connecting','Connecting...');btn('<i class="fas fa-spinner fa-spin"></i> Connecting','',1);
 try{
  const r=await fetch(`http://${ip}/data`,{signal:AbortSignal.timeout(5000)});const j=await r.json();
  if(j.cells===undefined)throw 0;
  S.live=true;S.fails=0;S.t0=Date.now();status('connected','Connected');btn('<i class="fas fa-plug-circle-xmark"></i> Disconnect','dc');
  $('ss').textContent='ESP32 Live';toast('Connected: '+ip,'success');
  clearInterval(S.poll);S.poll=setInterval(pull,1000);
 }catch(e){
  status('error','Cannot Reach',`Cannot reach ${ip}. Same Wi-Fi? Correct IP? ESP32 running?`);btn('<i class="fas fa-plug"></i> Connect');toast('Connection failed','error');
 }
}
async function pull(){
 try{const r=await fetch(`http://${S.ip}/data`,{signal:AbortSignal.timeout(3000)});const j=await r.json();if(j.cells!==undefined){S.fails=0;upd(j)}}
 catch(e){if(++S.fails>=5){disc();status('error','Connection Lost','ESP32 response varala. Check power / Wi-Fi.');toast('ESP32 connection lost','error')}}
}
function disc(){S.live=false;clearInterval(S.poll);status('idle','Not Connected');btn('<i class="fas fa-plug"></i> Connect');$('ss').textContent='None';resetLive()}


function upd(d){
 S.d=d;S.n++;
 const c=d.cells||[],t=d.temps||[];
 $('v-pack').textContent=num(d.pack,2);
 $('tr-imb').textContent='Δ '+num(d.imbalance,2)+' V imbalance';$('tr-imb').style.color=d.imbalance>0.2?'var(--am)':'var(--t3)';
 [0,1,2].forEach(i=>{let e=$('c'+(i+1));e.textContent=num(c[i],2);e.style.color=col((d.cell_s||[])[i]);e=$('t'+(i+1));e.textContent=num(t[i],1);e.style.color=col((d.temp_s||[])[i])});
 $('v-cur').textContent=num(d.current_ma,0);st('s-cur',d.cur_s);$('v-pw').textContent=num(d.power_mw,0)+' mW';$('v-bus').textContent=num(d.bus_v,2)+' V bus';
 $('v-vib').textContent=num(d.vib,2);st('s-vib',d.vib_s);$('v-tilt').textContent='tilt '+num(d.tilt,0)+'°';st('s-tilt',d.tilt_s);$('v-rp').textContent=`roll ${num(d.roll,0)}° pitch ${num(d.pitch,0)}°`;
 $('v-noise').textContent=num(d.noise_db,0);st('s-noise',d.noise_s);
 $('v-oil').textContent=num(d.oil_pct,0);st('s-oil',d.oil_s);$('b-oil').style.width=Math.min(100,d.oil_pct||0)+'%';
 $('v-metal').textContent=d.metal?'DETECTED':'NOT DETECTED';$('v-metal').style.color=col(d.metal_s);
 $('v-ins').textContent=d.insulation?'OK':'FAIL';$('v-ins').style.color=col(d.insul_s);
 const H=S.h,p=(k,v)=>{H[k].push(v);if(H[k].length>60)H[k].shift()};
 p('c1',c[0]);p('c2',c[1]);p('c3',c[2]);p('t1',t[0]);p('t2',t[1]);p('t3',t[2]);p('cur',d.current_ma);p('vib',d.vib);p('noise',d.noise_db);p('pack',d.pack);p('l',new Date().toLocaleTimeString());
 S.pkT=Math.max(S.pkT,...t.filter(x=>x!=null));S.pkI=Math.max(S.pkI,d.current_ma||0);
 $('sp').textContent=S.n.toLocaleString();$('su').textContent=d.uptime_s<60?d.uptime_s+'s':Math.floor(d.uptime_s/60)+'m '+d.uptime_s%60+'s';
 $('spk').textContent=S.pkT.toFixed(1)+'°C';$('sav').textContent=S.pkI.toFixed(0)+' mA';
 alerts(d);health(d);charts();
}

function alerts(d){
 const now=new Date().toLocaleTimeString(),L=[];
 (d.cell_s||[]).forEach((s,i)=>L.push([s,'cell'+i,`Cell ${i+1} ${num(d.cells[i],2)} V ${s}`]));
 (d.temp_s||[]).forEach((s,i)=>L.push([s,'temp'+i,`Temperature T${i+1} ${num(d.temps[i],1)} °C ${s}`]));
 L.push([d.cur_s,'cur',`Current ${num(d.current_ma,0)} mA ${d.cur_s}`],[d.tilt_s,'tilt',`Tilt ${num(d.tilt,0)}° ${d.tilt_s}`],[d.vib_s,'vib',`Vibration ${num(d.vib,2)} m/s² ${d.vib_s}`],
  [d.noise_s,'noise',`Noise ${num(d.noise_db,0)} dB ${d.noise_s}`],[d.oil_s,'oil',`Oil purity ${num(d.oil_pct,0)} % ${d.oil_s}`],
  [d.metal_s,'metal','Metal wear debris detected'],[d.insul_s,'ins','Insulation fault detected']);
 if(d.imbalance>0.2)L.push(['WARN','imb',`Cell imbalance ${num(d.imbalance,2)} V`]);
 L.forEach(([s,k,m])=>{if(!s||OKS.includes(s)||s==='ERR')return;const l=S.al.find(x=>x.k===k);if(l&&Date.now()-l.ts<8000)return;S.al.unshift({ty:REDS.includes(s)?'c':'w',k,m,time:now,ts:Date.now()})});
 S.al.length=Math.min(S.al.length,50);drawAl();
}
const alH=a=>`<div class="al ${a.ty==='c'?'':'w'}"><i class="fas ${a.ty==='c'?'fa-circle-exclamation':'fa-triangle-exclamation'}" style="color:${a.ty==='c'?'var(--rd)':'var(--am)'};margin-top:2px"></i><div><b>${a.m}</b><small>${a.time}</small></div></div>`;
function drawAl(){const n=S.al.length;$('ac').textContent=n;$('ac').style.display=n?'':'none';
 const e='<div class="empty">No alerts — system monitoring active</div>';
 $('ra').innerHTML=n?S.al.slice(0,3).map(alH).join(''):e;$('aa').innerHTML=n?S.al.map(alH).join(''):e}
function clearAl(){S.al=[];drawAl();toast('Alerts cleared')}

function health(d){
 const r=$('rfg');
 if(!S.live||!d){r.setAttribute('stroke-dashoffset',490);r.style.stroke='#1e2d4a';$('hs').textContent='--';$('hs').style.color='var(--t3)';$('hl').textContent='Waiting for ESP32';$('hl').style.color='var(--t3)';return}
 let s=100;
 [...(d.cell_s||[]),...(d.temp_s||[]),d.cur_s,d.tilt_s,d.vib_s,d.noise_s,d.oil_s,d.metal_s,d.insul_s].forEach(x=>{if(!x||x==='ERR'||OKS.includes(x))return;s-=REDS.includes(x)?20:8});
 if(d.imbalance>0.2)s-=10;
 s=Math.max(0,Math.min(100,s));
 const c=s>=60?'#00e68a':s>=40?'#ffb020':'#ff4060',lab=s>=80?'Excellent':s>=60?'Good':s>=40?'Fair':'Critical';
 r.setAttribute('stroke-dashoffset',490-s/100*490);r.style.stroke=c;
 $('hs').textContent=Math.round(s);$('hs').style.color=c;$('hl').textContent=lab;$('hl').style.color=c;
}

const CH={};
function mk(id,sets,legend){
 const mini=id==='c-mini';
 CH[id]=new Chart($(id),{type:'line',data:{labels:[],datasets:sets.map(([c,l])=>({label:l,data:[],borderColor:c,backgroundColor:c+'22',fill:sets.length===1,tension:.4,pointRadius:0,borderWidth:2,spanGaps:false}))},
  options:{responsive:true,maintainAspectRatio:false,animation:false,plugins:{legend:{display:!!legend,labels:{color:'#8899b4',boxWidth:12}}},
  scales:mini?{x:{display:false},y:{display:false}}:{x:{grid:{color:'rgba(30,45,74,.5)'},ticks:{color:'#5a6f92',font:{size:9},maxTicksLimit:5,maxRotation:0}},y:{grid:{color:'rgba(30,45,74,.5)'},ticks:{color:'#5a6f92',font:{size:10},maxTicksLimit:6}}}}});
}
function initCharts(){
 mk('c-cells',[['#60a5fa','Cell 1'],['#00e68a','Cell 2'],['#c084fc','Cell 3']],1);
 mk('c-temps',[['#ff6432','T1'],['#facc15','T2'],['#2dd4bf','T3']],1);
 mk('c-cur',[['#facc15','Current mA']]);
 mk('c-vn',[['#2dd4bf','Vibration m/s²'],['#ff6432','Noise dB']],1);
 mk('c-mini',[['#60a5fa']]);
}
function setC(id,l,...ys){const c=CH[id];c.data.labels=l;ys.forEach((y,i)=>c.data.datasets[i].data=y);c.update('none')}
function charts(){
 const h=S.h;setC('c-mini',h.l.slice(-30),h.pack.slice(-30));
 if($('t-charts').classList.contains('on')){setC('c-cells',h.l,h.c1,h.c2,h.c3);setC('c-temps',h.l,h.t1,h.t2,h.t3);setC('c-cur',h.l,h.cur);setC('c-vn',h.l,h.vib,h.noise)}
}
function resetLive(){
 S.d=null;S.n=0;S.pkT=0;S.pkI=0;Object.keys(S.h).forEach(k=>S.h[k]=[]);
 $('cards').innerHTML=CARDS;
 $('sp').textContent='0';['su','spk','sav'].forEach(i=>$(i).textContent='--');
 health(null);Object.keys(CH).forEach(id=>setC(id,[],...CH[id].data.datasets.map(()=>[])));
}

// AI inspection (calls Flask /api/analyze)
const drop=$('drop');
drop.ondragover=e=>{e.preventDefault();drop.classList.add('dg')};drop.ondragleave=()=>drop.classList.remove('dg');
drop.ondrop=e=>{e.preventDefault();drop.classList.remove('dg');e.dataTransfer.files[0]&&load(e.dataTransfer.files[0])};
$('fi').onchange=e=>e.target.files[0]&&load(e.target.files[0]);
function load(f){
 if(!f.type.startsWith('image/'))return toast('Image file upload pannunga','error');
 S.file=f;const r=new FileReader();r.onload=e=>{S.img=e.target.result;$('pi').src=S.img;$('pn').textContent=f.name;$('dc').classList.add('hide');$('pv').classList.remove('hide');$('ba').classList.remove('hide')};r.readAsDataURL(f);
}
async function analyze(){
 if(!S.file)return;
 $('ir').innerHTML='<div class="empty"><i class="fas fa-spinner fa-spin" style="font-size:26px;color:var(--ac);display:block;margin-bottom:10px"></i>AI inspection nadakkudhu...</div>';
 $('ba').disabled=true;
 const fd=new FormData();fd.append('image',S.file);fd.append('part',$('part').value);
 if(S.live)fd.append('sensors',JSON.stringify(S.d));
 try{
  const r=await fetch('/api/analyze',{method:'POST',body:fd}),j=await r.json();
  if(!r.ok)throw new Error(j.error||'Server error');
  showResult(j);
 }catch(e){
  $('ir').innerHTML=`<div class="empty" style="color:var(--rd)"><i class="fas fa-circle-exclamation" style="font-size:26px;display:block;margin-bottom:10px"></i>${e.message}</div>`;
  toast('Inspection fail','error');
 }
 $('ba').disabled=false;
}
function showResult(j){
 if(j.verdict==='REJECTED'){
  $('ir').innerHTML=`<div class="empty" style="color:var(--am)"><i class="fas fa-ban" style="font-size:26px;display:block;margin-bottom:10px"></i>Idhu <b>${j.part_label}</b> photo illa.<br>AI paarthadhu: ${j.detected||'-'}</div>`;
  return toast('Wrong part photo','error');
 }
 const sc=j.score,col=j.verdict==='PASS'?'var(--ac)':j.verdict==='CAUTION'?'var(--am)':'var(--rd)',tg={ok:'ok',warning:'wr',critical:'cr'},tx={ok:'OK',warning:'Warning',critical:'Critical'};
 $('ir').innerHTML=`<div class="sh"><div><div class="fd" style="font-size:28px;font-weight:700;color:${col}">${sc??'--'}/100</div><small style="color:var(--t3)">${j.part_label}</small></div><span class="tag ${j.verdict==='PASS'?'ok':j.verdict==='CAUTION'?'wr':'cr'}">${j.verdict}</span></div>
 <p style="font-size:12px;color:var(--t3);margin-bottom:12px">AI detected: ${j.detected||'-'}</p>`+
 j.findings.map(p=>`<div class="part"><div class="sh"><b style="font-size:14px">${p.item}</b><span class="tag ${tg[p.status]}">${tx[p.status]}</span></div><p>${p.detail}</p></div>`).join('')+
 `<div class="part"><b style="font-size:13px">Recommended action</b><p style="margin-top:4px">${j.advice||'-'}</p></div>`+
 (j.pdf?`<a class="btn" href="${j.pdf}" target="_blank"><i class="fas fa-file-pdf"></i> Download PDF Report</a>`:'');
 S.ins.unshift({t:new Date().toLocaleString(),sc:sc??0,n:j.findings.filter(p=>p.status!=='ok').length,part:j.part_label});
 $('ih').innerHTML=S.ins.map(i=>`<div class="part sh" style="margin-bottom:8px"><span style="font-size:13px">${i.part} · ${i.t}</span><span><b style="color:${i.sc>=70?'var(--ac)':'var(--am)'}">${i.sc}/100</b> <span class="tag ${i.n?'wr':'ok'}">${i.n} issue${i.n!==1?'s':''}</span></span></div>`).join('');
 toast('Inspection complete','success');
}

// tabs
const T={live:['Live Sensor Monitor','Real-time vehicle vital signs from ESP32'],charts:['Analytics','Historical data trends'],inspect:['AI Visual Inspection','Upload photos for fault detection'],alerts:['Alerts & Thresholds','Review alerts and set limits'],link:['ESP32 Connection','Connect to your sensor board'],info:['System Info','Data format and troubleshooting']};
function go(t){
 $$('.tab').forEach(e=>e.classList.remove('on'));$$('.ni').forEach(e=>e.classList.toggle('on',e.dataset.t===t));
 $('t-'+t).classList.add('on');$('pt').textContent=T[t][0];$('ps').textContent=T[t][1];
 scrollTo(0,0);if(t==='charts')charts();
}
$$('.ni').forEach(b=>b.onclick=()=>go(b.dataset.t));

function clock(){const n=new Date();$('ck').textContent=n.toLocaleTimeString();$('cd').textContent=n.toLocaleDateString('en-US',{weekday:'short',year:'numeric',month:'short',day:'numeric'})}
let started=false;
function enter(){
 $('welcome').classList.add('out');setTimeout(()=>$('welcome').classList.add('hide'),550);
 if(started)return;started=true;
 initCharts();clock();setInterval(clock,1000);drawAl();
 resetLive();
}
document.addEventListener('keydown',e=>{if(e.key==='Enter'&&!$('welcome').classList.contains('hide'))enter()});

