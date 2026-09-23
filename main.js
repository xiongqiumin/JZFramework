const p = new URL('https://example.com:8443/a/b/c.html?q=1&x=plain');
console.log('clean=' + p.hostname + '|' + p.port + '|' + p.pathname + '|' + p.searchParams.get('x') + '|' + p.search + '|' + p.protocol);
const q = new URL('http://127.0.0.1:3900/x?y=7');
console.log('q=' + q.hostname + '|' + q.port + '|' + q.pathname + '|' + q.searchParams.get('y') + '|' + q.searchParams.toString());
console.log('sp=' + new URLSearchParams('a=1&b=two').toString() + '|' + new URLSearchParams({a:1}).get('a'));