const { toBeType } = require('jest-tobetype');
const mediasoup = require('../');
const { createWorker } = mediasoup;
const { InvalidStateError } = require('../lib/errors');

expect.extend({ toBeType });

let worker;

beforeEach(() => worker && !worker.closed && worker.close());
afterEach(() => worker && !worker.closed && worker.close());

const mediaCodecs =
[
	{
		kind       : 'audio',
		name       : 'opus',
		mimeType   : 'audio/opus',
		clockRate  : 48000,
		channels   : 2,
		parameters :
		{
			useinbandfec : 1,
			foo          : 'bar'
		}
	},
	{
		kind      : 'video',
		name      : 'VP8',
		clockRate : 90000
	},
	{
		kind         : 'video',
		name         : 'H264',
		mimeType     : 'video/H264',
		clockRate    : 90000,
		rtcpFeedback : [], // Will be ignored.
		parameters   :
		{
			'level-asymmetry-allowed' : 1,
			'packetization-mode'      : 1,
			'profile-level-id'        : '4d0032'
		}
	}
];

test('worker.createRouter() succeeds', async () =>
{
	worker = await createWorker();

	const router = await worker.createRouter({ mediaCodecs });

	expect(router.id).toBeType('string');
	expect(router.closed).toBe(false);
	expect(router.rtpCapabilities).toBeType('object');
	expect(router.rtpCapabilities.codecs).toBeType('array');
	expect(router.rtpCapabilities.headerExtensions).toBeType('array');
	expect(router.rtpCapabilities.fecMechanisms).toEqual([]);

	await expect(worker.dump())
		.resolves
		.toEqual({ pid: worker.pid, routerIds: [ router.id ] });

	await expect(router.dump())
		.resolves
		.toMatchObject(
			{
				id                       : router.id,
				transportIds             : [],
				mapProducerIdConsumerIds : {},
				mapConsumerIdProducerId  : {}
			});

	// Private API.
	expect(worker._routers.size).toBe(1);

	worker.close();

	expect(router.closed).toBe(true);

	// Private API.
	expect(worker._routers.size).toBe(0);
}, 1000);

test('worker.createRouter() without mediaCodecs rejects with TypeError', async () =>
{
	worker = await createWorker();

	await expect(worker.createRouter())
		.rejects
		.toThrow(TypeError);

	worker.close();
}, 1000);

test('worker.createRouter() rejects with InvalidStateError if Worker is closed', async () =>
{
	worker = await createWorker();

	worker.close();

	await expect(worker.createRouter({ mediaCodecs }))
		.rejects
		.toThrow(InvalidStateError);
}, 1000);

test('Router emits "workerclose" if Worker is closed', async () =>
{
	worker = await createWorker();

	const router = await worker.createRouter({ mediaCodecs });

	await new Promise((resolve) =>
	{
		router.on('workerclose', resolve);

		worker.close();
	});

	expect(router.closed).toBe(true);
}, 1000);
