print('Hallo aus SpiderMonkey auf QRX OS One 64: ' + (6 * 7));
print([3, 1, 2].sort().join('-') + ' ' + JSON.stringify({ a: [1, 2], b: 'x' }));
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
print('fib(20) = ' + fib(20));
print(new Date(0).toISOString() + ' ' + Math.sqrt(2).toFixed(6) + ' ' + /a(b+)c/.exec('xabbbc')[1]);
print(Array.from({ length: 5 }, (_, i) => i * i).map(String).join(','));
print(typeof BigInt === 'function' ? (2n ** 64n).toString() : 'no BigInt');
