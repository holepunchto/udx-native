const test = require('brittle')
const b4a = require('b4a')
const UDX = require('../')
const { uncaught, createSocket } = require('./helpers')

test('can bind and close', async function (t) {
  const u = new UDX()
  const s = createSocket(t, u)

  s.bind(0, '127.0.0.1')
  await s.close()

  t.pass()
})

test('can bind to ipv6 and close', async function (t) {
  const u = new UDX()
  const s = createSocket(t, u)

  s.bind(0, '::1')
  await s.close()

  t.pass()
})

test('bind is effectively sync', async function (t) {
  const u = new UDX()

  const a = createSocket(t, u)
  const b = createSocket(t, u)

  a.bind(0, '127.0.0.1')

  t.ok(a.address().port, 'has bound')
  t.exception(() => b.bind(a.address().port, '127.0.0.1'))

  await a.close()
  await b.close()
})

test('simple message', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = createSocket(t, u)

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0, '127.0.0.1')
  await a.send(b4a.from('hello'), a.address().port)
})

test('simple message ipv6', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = createSocket(t, u)

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '::1')
    t.is(family, 6)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0, '::1')
  await a.send(b4a.from('hello'), a.address().port, '::1')
})

test('empty message', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  a.on('message', function (message) {
    t.alike(message, b4a.alloc(0))
    a.close()
  })

  a.bind(0, '127.0.0.1')
  await a.send(b4a.alloc(0), a.address().port)
})

test('handshake', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = createSocket(t, u)
  const b = createSocket(t, u)

  t.teardown(async () => {
    await a.close()
    await b.close()
  })

  a.once('message', function (message) {
    t.alike(message, b4a.alloc(1))
  })

  b.once('message', function (message) {
    t.alike(message, b4a.alloc(0))
  })

  a.bind(0, '127.0.0.1')
  b.bind(0, '127.0.0.1')

  a.trySend(b4a.alloc(0), b.address().port)
  b.trySend(b4a.alloc(1), a.address().port)
})

test('echo sockets (250 messages)', async function (t) {
  t.plan(3)

  const u = new UDX()

  const a = createSocket(t, u)
  const b = createSocket(t, u)

  const send = []
  const recv = []
  let echoed = 0
  let flushed = 0

  a.on('message', function (buf, { host, port }) {
    echoed++
    a.send(buf, port, host)
  })

  b.on('message', function (buf) {
    recv.push(buf)

    if (recv.length === 250) {
      t.alike(send, recv)
      t.is(echoed, 250)
      t.is(flushed, 250)
      a.close()
      b.close()
    }
  })

  a.bind(0, '127.0.0.1')

  while (send.length < 250) {
    const buf = b4a.from('a message')
    send.push(buf)
    b.send(buf, a.address().port).then(function () {
      flushed++
    })
  }
})

test('close socket while sending', async function (t) {
  const u = new UDX()
  const a = createSocket(t, u)

  a.bind(0, '127.0.0.1')
  const flushed = a.send(b4a.from('hello'), a.address().port)

  a.close()

  t.is(await flushed, false)
})

test('close waits for all streams to close', async function (t) {
  t.plan(2)

  const u = new UDX()

  // just so we can a udx port, to avoid weird failures
  const dummy = createSocket(t, u)
  dummy.bind(0, '127.0.0.1')
  t.teardown(() => dummy.close())

  const a = createSocket(t, u)
  const s = u.createStream(1)

  s.connect(a, 2, dummy.address().port)

  let aClosed = false
  let sClosed = false

  s.on('close', function () {
    t.not(aClosed, 'socket waits for streams')
    sClosed = true
  })

  a.on('close', function () {
    t.ok(sClosed, 'stream was closed before socket')
    aClosed = true
  })

  a.close()

  setTimeout(function () {
    s.destroy()
  }, 100)
})

test('open + close a bunch of sockets', async function (t) {
  const u = new UDX()

  const l = t.test('linear')
  let count = 0

  l.plan(5)
  loop()

  async function loop () {
    count++

    const a = createSocket(t, u)

    a.bind(0, '127.0.0.1')
    l.pass('opened socket')
    await a.close()

    if (count < 5) loop()
  }

  await l

  const p = t.test('parallel')
  p.plan(5)

  for (let i = 0; i < 5; i++) {
    const a = createSocket(t, u)
    a.bind(0, '127.0.0.1')
    a.close().then(function () {
      p.pass('opened and closed socket')
    })
  }

  await p
})

test('can bind to ipv6 and receive from ipv4', async function (t) {
  t.plan(4)

  const u = new UDX()

  const a = createSocket(t, u)
  const b = createSocket(t, u)

  a.on('message', async function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, b.address().port)
    a.close()
    b.close()
  })

  a.bind(0, '::')
  b.bind(0, '127.0.0.1')

  b.send(b4a.from('hello'), a.address().port, '127.0.0.1')
})

test('can bind to ipv6 and send to ipv4', async function (t) {
  t.plan(4)

  const u = new UDX()

  const a = createSocket(t, u)
  const b = createSocket(t, u)

  b.on('message', async function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, a.address().port)
    a.close()
    b.close()
  })

  a.bind(0, '::')
  b.bind(0, '127.0.0.1')

  a.send(b4a.from('hello'), b.address().port, '127.0.0.1')
})

test('can bind to ipv6 only and not receive from ipv4', async function (t) {
  t.plan(1)

  const u = new UDX()

  const a = createSocket(t, u, { ipv6Only: true })
  const b = createSocket(t, u)

  a.on('message', async function () {
    t.fail('a received message')
  })

  a.bind(0, '::')
  b.bind(0, '127.0.0.1')

  b.send(b4a.from('hello'), a.address().port, '127.0.0.1')

  setTimeout(() => {
    a.close()
    b.close()

    t.pass()
  }, 100)
})

test('send after close', async function (t) {
  const u = new UDX()

  const a = createSocket(t, u)

  a.bind(0, '127.0.0.1')
  a.close()

  t.is(await a.send(b4a.from('hello'), a.address().port), false)
})

test('try send simple message', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = createSocket(t, u)

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0, '127.0.0.1')
  a.trySend(b4a.from('hello'), a.address().port)
})

test('try send simple message ipv6', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = createSocket(t, u)

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '::1')
    t.is(family, 6)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0, '::1')
  a.trySend(b4a.from('hello'), a.address().port, '::1')
})

test('try send empty message', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  a.on('message', function (message) {
    t.alike(message, b4a.alloc(0))
    a.close()
  })

  a.bind(0, '127.0.0.1')
  a.trySend(b4a.alloc(0), a.address().port)
})

test('close socket while try sending', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = createSocket(t, u)

  a.bind(0, '127.0.0.1')

  a.on('message', function (message) {
    t.fail('should not receive message')
  })

  a.on('close', function () {
    t.pass()
  })

  t.is(a.trySend(b4a.from('hello'), a.address().port), undefined)

  a.close()
})

test('try send after close', async function (t) {
  t.plan(2)

  const u = new UDX()

  const a = createSocket(t, u)

  a.on('message', function (message) {
    t.fail('should not receive message')
  })

  a.on('close', function () {
    t.pass()
  })

  a.bind(0, '127.0.0.1')
  a.close()

  t.is(a.trySend(b4a.from('hello'), a.address().port), undefined)
})

test('connect to invalid host ip', async function (t) {
  t.plan(1)

  const u = new UDX()

  const a = createSocket(t, u)
  const s = u.createStream(1)

  const invalidHost = '0.-1.0.0'

  try {
    s.connect(a, 2, 0, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }

  s.destroy()

  await a.close()
})

test('bind to invalid host ip', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  const invalidHost = '0.-1.0.0'

  try {
    a.bind(0, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }

  await a.close()
})

test('send to invalid host ip', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  a.bind(0, '127.0.0.1')

  const invalidHost = '0.-1.0.0'

  try {
    await a.send(b4a.from('hello'), a.address().port, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }

  await a.close()
})

test('try send to invalid host ip', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  a.bind(0, '127.0.0.1')

  const invalidHost = '0.-1.0.0'

  try {
    a.trySend(b4a.from('hello'), a.address().port, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }

  await a.close()
})

test('send without bind', async function (t) {
  t.plan(1)

  const u = new UDX()

  const a = createSocket(t, u)
  const b = createSocket(t, u)

  b.on('message', function (message) {
    t.alike(message, b4a.from('hello'))
    a.close()
    b.close()
  })

  b.bind(0, '127.0.0.1')
  await a.send(b4a.from('hello'), b.address().port)
})

test('try send without bind', async function (t) {
  t.plan(1)

  const u = new UDX()

  const a = createSocket(t, u)
  const b = createSocket(t, u)

  b.on('message', function (message) {
    t.alike(message, b4a.from('hello'))
    a.close()
    b.close()
  })

  b.bind(0, '127.0.0.1')
  a.trySend(b4a.from('hello'), b.address().port)
})

test('throw in message callback', async function (t) {
  t.plan(1)

  const u = new UDX()

  const a = createSocket(t, u)
  const b = createSocket(t, u)

  a.on('message', function () {
    throw new Error('boom')
  })

  a.bind(0, '127.0.0.1')

  b.send(b4a.from('hello'), a.address().port)

  uncaught(async (err) => {
    t.is(err.message, 'boom')

    await a.close()
    await b.close()
  })
})

test('get address without bind', async function (t) {
  const u = new UDX()
  const a = createSocket(t, u)
  t.is(a.address(), null)
  await a.close()
})

test('bind twice', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  a.bind(0, '127.0.0.1')

  try {
    a.bind(0, '127.0.0.1')
  } catch (error) {
    t.is(error.message, 'Already bound')
  }

  await a.close()
})

test('bind while closing', function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  a.close()

  try {
    a.bind(0, '127.0.0.1')
  } catch (error) {
    t.is(error.message, 'Socket is closed')
  }
})

test('different socket binds to same host and port', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)
  const b = createSocket(t, u)

  a.bind(0, '0.0.0.0')

  try {
    b.bind(a.address().port, '0.0.0.0')
  } catch {
    t.pass()
  }

  await a.close()
  await b.close()
})

test('different socket binds to default host but same port', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)
  const b = createSocket(t, u)

  a.bind()

  try {
    b.bind(a.address().port)
  } catch {
    t.pass()
  }

  await a.close()
  await b.close()
})

test('close twice', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = createSocket(t, u)

  a.bind(0, '127.0.0.1')

  a.on('close', function () {
    t.pass()
  })

  a.close()
  a.close()
})

test('set TTL', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = createSocket(t, u)

  a.on('message', function (message) {
    t.alike(message, b4a.from('hello'))
    a.close()
  })

  try {
    a.setTTL(5)
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0, '127.0.0.1')
  a.setTTL(5)

  await a.send(b4a.from('hello'), a.address().port)
})

test('get recv buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = createSocket(t, u)

  try {
    a.getRecvBufferSize()
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0, '127.0.0.1')
  t.ok(a.getRecvBufferSize() > 0)

  await a.close()
})

test('set recv buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = createSocket(t, u)

  const NEW_BUFFER_SIZE = 8192

  try {
    a.setRecvBufferSize(NEW_BUFFER_SIZE)
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0, '127.0.0.1')
  a.setRecvBufferSize(NEW_BUFFER_SIZE)

  t.ok(a.getRecvBufferSize() >= NEW_BUFFER_SIZE)

  await a.close()
})

test('get send buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = createSocket(t, u)

  try {
    a.getSendBufferSize()
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0, '127.0.0.1')
  t.ok(a.getSendBufferSize() > 0)

  await a.close()
})

test('set send buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = createSocket(t, u)

  const NEW_BUFFER_SIZE = 8192

  try {
    a.setSendBufferSize(NEW_BUFFER_SIZE)
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0, '127.0.0.1')
  a.setSendBufferSize(NEW_BUFFER_SIZE)

  t.ok(a.getSendBufferSize() >= NEW_BUFFER_SIZE)

  await a.close()
})

test('UDX - socket stats 0 before bind is called', async function (t) {
  const a = new UDX()

  const aSocket = a.createSocket()

  t.is(aSocket.bytesTransmitted, 0)
  t.is(aSocket.packetsTransmitted, 0)
  t.is(aSocket.bytesReceived, 0)
  t.is(aSocket.packetsReceived, 0)
})
