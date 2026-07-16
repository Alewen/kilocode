bun -e @"
const {Database}=require('bun:sqlite');
const p=process.env.USERPROFILE;
const db=new Database(p+'\\.local\\share\\kilo\\kilo.db');
const r=db.query('SELECT version, directory, COUNT(*) as cnt FROM session GROUP BY version, directory ORDER BY version, directory').all();
console.log('version | directory | count');
console.log('--------|-----------|------');
for(const s of r) console.log(s.version+' | '+s.directory+' | '+s.cnt);
"@