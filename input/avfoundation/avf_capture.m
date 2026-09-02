#if defined(__APPLE__)

#import "avf_capture.h"

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>

@interface AVRecorderDocument : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
@private
	AVCaptureSession         *session;
	AVCaptureDeviceInput     *videoDeviceInput;
	AVCaptureVideoDataOutput *videoDataOutput;
	dispatch_queue_t          queue;
}

- (int)start;
- (int)stop;

- (void)captureOutput:(AVCaptureOutput *)captureOutput didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
	fromConnection:(AVCaptureConnection *)connection;

@end

@implementation AVRecorderDocument

- (id)init
{
	self = [super init];

	session = [[AVCaptureSession alloc] init];

	AVCaptureDevice *videoDevice = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
	if (!videoDevice) {
		return NULL;
	}

	NSLog(@"Using video device: %@\n", videoDevice.localizedName);
	NSLog(@"           formats: %@\n", videoDevice.formats);
	NSLog(@"     Active format: %@\n", videoDevice.activeFormat);

	NSError *error = nil;
	videoDeviceInput = [AVCaptureDeviceInput deviceInputWithDevice:videoDevice error:&error];
	if (!videoDeviceInput) {
		NSLog(@"Unable to create video device input: %@\n", error);
		return NULL;
	}

	[session beginConfiguration];
	[session setSessionPreset:AVCaptureSessionPresetHigh];
	[session addInput:videoDeviceInput];

	videoDataOutput = [[AVCaptureVideoDataOutput alloc] init];
	videoDataOutput.alwaysDiscardsLateVideoFrames = NO;

	queue = dispatch_queue_create("cameraQueue", NULL);
	[videoDataOutput setSampleBufferDelegate:self queue:queue];

#if 0
/* If we want BGRA */
	NSString *key = (NSString *)kCVPixelBufferPixelFormatTypeKey; 
	NSNumber *value = [NSNumber numberWithUnsignedInt:kCVPixelFormatType_32BGRA]; 
	NSDictionary *videoSettings = [NSDictionary dictionaryWithObject:value forKey:key]; 
	[videoDataOutput setVideoSettings:videoSettings]; 
#else
	/* We'll get 2YUV */
#endif
	[session addOutput:videoDataOutput];

	[session commitConfiguration];

	[session startRunning];

	return 0; /* OK */
}

- (int)start
{
	[session startRunning];
	printf("Start running\n");
	return 0; /* OK */
}

- (int)stop
{
	[session stopRunning];
	printf("Stop running\n");
	return 0; /* OK */
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput
	didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
	fromConnection:(AVCaptureConnection *)connection
{
	NSLog(@"sampleBuffer %@\n", sampleBuffer);
	NSLog(@"sample.size = %lu\n", CMSampleBufferGetTotalSampleSize(sampleBuffer));

	CMTime t = CMSampleBufferGetDecodeTimeStamp(sampleBuffer);
	NSLog(@"time = %" PRIi64 " / %d\n", t.value, t.timescale);
}

@end

AVRecorderDocument *doc;

int avf_capture_init(void)
{
	doc = [[AVRecorderDocument alloc] init];
	return 0; /* OK */
}

int avf_capture_start(void)
{
	[doc start];
	return 0; /* OK */
}

int avf_capture_stop(void)
{
	[doc stop];
	return 0; /* OK */
}

#endif /* #if defined(__APPLE__) */
