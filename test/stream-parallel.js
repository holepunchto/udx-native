const test = require('brittle')
const { makePairs, pipeStreamPairs } = require('./helpers')

test('16 parallel streams on 1 socket', async function (t) {
  t.timeout(90000)
  const { streams, close } = makePairs(16, 'single')
  t.teardown(close)
  t.plan(1)
  const messageSize = 1024 * 64
  const limit = 1024 * 512

  await t.execution(pipeStreamPairs(streams, messageSize, limit))
})

test('16 parallel streams on 16 sockets', async function (t) {
  t.timeout(90000)
  const { streams, close } = makePairs(16, 'multi')
  t.teardown(close)
  t.plan(1)
  const messageSize = 1024 * 64
  const limit = 1024 * 512

  await t.execution(pipeStreamPairs(streams, messageSize, limit))
})
