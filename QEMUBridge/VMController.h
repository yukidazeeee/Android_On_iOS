#pragma once
#import <UIKit/UIKit.h>
#import "ADB/ADBClient.h"
NS_ASSUME_NONNULL_BEGIN
@interface AEVMController : UIViewController
@property(nonatomic, copy, nullable) void (^showControls)(void);
@property(nonatomic, readonly, nullable) AEADBClient *adb;
@property(nonatomic, readonly) NSString *statusText;
@property(nonatomic, readonly) NSString *serialText;
@property(nonatomic, readonly) NSString *graphicsDiagnosticsText;
@property(nonatomic, readonly) BOOL engineAvailable;
@property(nonatomic, readonly) BOOL started;
@property(nonatomic, readonly) BOOL stopped;
@property(nonatomic, readonly) BOOL guestPaused;
- (BOOL)startWithImageDirectory:(NSString *)path
                            ramMiB:(uint32_t)ram
                          cacheMiB:(uint32_t)cache
                        panelWidth:(uint32_t)width
                       panelHeight:(uint32_t)height
                          apiLevel:(uint32_t)apiLevel
    NS_SWIFT_NAME(start(imageDirectory:ramMiB:cacheMiB:panelWidth:panelHeight:apiLevel:));
- (void)setGuestPaused:(BOOL)paused;
- (void)stopGuest;
- (void)sendGuestKey:(uint16_t)code pressed:(BOOL)pressed NS_SWIFT_NAME(sendGuestKey(_:pressed:));
- (void)clearGraphicsDiagnostics;
- (void)markGraphicsDiagnostics;
- (NSDictionary<NSString *, NSNumber *> *)statistics;
@end
NS_ASSUME_NONNULL_END
