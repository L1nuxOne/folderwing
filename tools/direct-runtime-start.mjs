// Detached Windows host bootstrap. Logs use files, not inherited launcher pipes.
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
const options={};
for(let i=2;i<process.argv.length;i+=2){const key=process.argv[i];if(!['--config','--stdout','--stderr'].includes(key)||!process.argv[i+1]||options[key])throw new Error('Invalid runtime bootstrap arguments');options[key]=process.argv[i+1];}
for(const key of ['--config','--stdout','--stderr'])if(!options[key])throw new Error('Missing runtime bootstrap argument');
const cfg=JSON.parse(fs.readFileSync(options['--config'],'utf8').replace(/^\uFEFF/,''));
for(const key of ['node','hostScript','profileRoot','projectRoot','projectId','model','reasoningEffort','authFile'])if(typeof cfg[key]!=='string'||!cfg[key])throw new Error('Invalid runtime configuration');
const args=[cfg.hostScript,'--profile-root',cfg.profileRoot,'--project-root',cfg.projectRoot,'--project-id',cfg.projectId,'--project-name',cfg.projectName||cfg.projectId,'--workspace-kind','local','--model',cfg.model,'--reasoning-effort',cfg.reasoningEffort,'--allow-live','--auth-file',cfg.authFile];
const opened=[];
try{
 const stdout=fs.openSync(options['--stdout'],'wx');opened.push(stdout);
 const stderr=fs.openSync(options['--stderr'],'wx');opened.push(stderr);
 const child=spawn(cfg.node,args,{cwd:path.dirname(options['--config']),detached:true,windowsHide:true,stdio:['ignore',stdout,stderr]});
 await new Promise((resolve,reject)=>{child.once('spawn',resolve);child.once('error',reject);});
 child.unref();console.log(JSON.stringify({pid:child.pid}));
}finally{for(const fd of opened)fs.closeSync(fd);}
