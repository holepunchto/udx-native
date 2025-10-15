const test = require('brittle')
const b4a = require('b4a')
const { makeTwoStreams } = require('./helpers')

test('relay', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t)
  const [c, d] = makeTwoStreams(t)

  c.relayTo(b)
  b.relayTo(c)

  a.on('data', function (data) {
    t.alike(data, b4a.from('hello world'))

    a.destroy()
    b.destroy()
    c.destroy()
    d.destroy()
  })

  d.write('hello world')
})

test('relay, destroy immediately', function (t) {
  const [a, b] = makeTwoStreams(t)
  const [c, d] = makeTwoStreams(t)

  c.relayTo(b)
  b.relayTo(c)

  a.destroy()
  b.destroy()
  c.destroy()
  d.destroy()

  t.pass()
})

test('relay, change remote', function (t) {
  t.plan(2)

  const [a, b] = makeTwoStreams(t)
  const [c, d] = makeTwoStreams(t)

  c.relayTo(b)
  b.relayTo(c)

  a.once('data', async function (data) {
    t.alike(data, b4a.from('hello world'))

    await a.changeRemote(a.socket, d.id, d.socket.address().port)
    await d.changeRemote(d.socket, a.id, a.socket.address().port)

    b.destroy()
    c.destroy()

    a.once('data', function (data) {
      t.alike(data, b4a.from('remote changed'))

      a.destroy()
      d.destroy()
    })

    d.write('remote changed')
  })

  d.write('hello world')
})

test('relay, change remote and destroy stream', function (t) {
  t.plan(2)

  const [a, b] = makeTwoStreams(t)
  const [c, d] = makeTwoStreams(t)

  c.relayTo(b)
  b.relayTo(c)

  a.once('data', async function (data) {
    t.alike(data, b4a.from('hello world'))

    const promises = [
      a.changeRemote(a.socket, d.id, d.socket.address().port),
      d.changeRemote(d.socket, a.id, a.socket.address().port)
    ]

    a.destroy()
    b.destroy()
    c.destroy()
    d.destroy()

    await Promise.allSettled(promises)

    t.pass()
  })

  d.write('hello world')
})

test('relay, throw if stream is closed', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t)

  a.on('close', () => {
    try {
      b.relayTo(a)
      t.fail('should fail')
    } catch (err) {
      t.ok(err)
      b.destroy()
    }
  }).destroy()
})

test('remote-changed emitted exactly once', async (t) => {
  t.plan(5)
  let aEmits = 0
  let dEmits = 0

  const [a, b] = makeTwoStreams(t)
  const [c, d] = makeTwoStreams(t)

  a.on('remote-changed', () => ++aEmits)
  d.on('remote-changed', () => ++dEmits)

  c.relayTo(b)
  b.relayTo(c)

  // send 'hello' d->c->b->a
  const pMsg1 = new Promise((resolve) => a.once('data', resolve))
  d.write('hello world')
  t.alike(await pMsg1, b4a.from('hello world'))

  // respond 'do change' a->b->c->d
  const pMsg2 = new Promise((resolve) => d.once('data', resolve))
  a.write('do change')
  t.alike(await pMsg2, b4a.from('do change'))

  // change
  await a.changeRemote(a.socket, d.id, d.socket.address().port)
  await d.changeRemote(d.socket, a.id, a.socket.address().port)

  // ack 'remote changed' d->a
  const pMsg3 = new Promise((resolve) => a.once('data', resolve))
  d.write('remote changed')
  t.alike(await pMsg3, b4a.from('remote changed'))

  a.destroy()
  d.destroy()
  b.destroy()
  c.destroy()

  t.is(aEmits, 1)
  t.is(dEmits, 1)
})
