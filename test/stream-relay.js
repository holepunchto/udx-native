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

  a
    .on('close', () => {
      try {
        b.relayTo(a)
        t.fail('should fail')
      } catch (err) {
        t.ok(err)
        b.destroy()
      }
    })
    .destroy()
})
