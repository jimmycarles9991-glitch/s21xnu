/* S21 Icon Probe: a tiny UIKit app whose images have known RGBA bytes.
 * It exists to exercise the same CoreAnimation/EAGL surfaces as app icons,
 * Clock-style sub-icons, folder previews and an on-screen keyboard. */
#import <UIKit/UIKit.h>

static NSArray<NSString *> *probe_assets(void)
{
    return @[@"probe180.png", @"clock28x65.png", @"clock28x95.png", @"clock28x28.png",
             @"clock6x85.png", @"clock6x6.png", @"folder1.png", @"folder2.png", @"folder3.png"];
}

static UIImage *probe_image(NSString *name)
{
    return [[UIImage imageNamed:name] imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
}

@interface DetailViewController : UIViewController
@property (nonatomic, strong) UIImage *image;
@property (nonatomic, copy) NSString *assetName;
@end

@implementation DetailViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];
    self.title = self.assetName;

    UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(16, 24, self.view.bounds.size.width - 32, 28)];
    label.text = self.assetName;
    label.font = [UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.view addSubview:label];

    UIImageView *view = [[UIImageView alloc] initWithImage:self.image];
    CGFloat side = MIN(self.view.bounds.size.width - 32, self.view.bounds.size.height - 120);
    view.frame = CGRectMake((self.view.bounds.size.width - side) / 2, 72, side, side);
    view.contentMode = UIViewContentModeScaleAspectFit;
    view.backgroundColor = [UIColor secondarySystemBackgroundColor];
    view.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
    [self.view addSubview:view];
}

@end

@interface ProbeListViewController : UITableViewController
@property (nonatomic, strong) NSArray<NSString *> *assets;
@end

@implementation ProbeListViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = @"RGBA";
    self.assets = probe_assets();
    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:@"asset"];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
    (void)tableView;
    (void)section;
    return (NSInteger)self.assets.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"asset" forIndexPath:indexPath];
    NSString *name = self.assets[(NSUInteger)indexPath.row];
    cell.textLabel.text = name;
    cell.imageView.image = probe_image(name);
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    DetailViewController *detail = [[DetailViewController alloc] init];
    detail.assetName = self.assets[(NSUInteger)indexPath.row];
    detail.image = probe_image(detail.assetName);
    [self.navigationController pushViewController:detail animated:YES];
}

@end

@interface TextFieldViewController : UIViewController
@end

@implementation TextFieldViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = @"Keyboard";
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(20, 40, self.view.bounds.size.width - 40, 90)];
    label.numberOfLines = 0;
    label.text = @"Tap here to bring up the keyboard. Small keyboard icons use the same wrapped picture path.";
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.view addSubview:label];

    UITextField *field = [[UITextField alloc] initWithFrame:CGRectMake(20, 150, self.view.bounds.size.width - 40, 44)];
    field.borderStyle = UITextBorderStyleRoundedRect;
    field.placeholder = @"Tap for keyboard";
    field.returnKeyType = UIReturnKeyDone;
    field.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.view addSubview:field];
}

@end

@interface GridViewController : UIViewController
@end

@implementation GridViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = @"Folder";
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    UIScrollView *scroll = [[UIScrollView alloc] initWithFrame:self.view.bounds];
    scroll.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:scroll];

    CGFloat side = 90;
    for (NSUInteger i = 0; i < 9; i++) {
        NSUInteger col = i % 3, row = i / 3;
        UIImageView *icon = [[UIImageView alloc] initWithImage:probe_image([NSString stringWithFormat:@"folder%lu.png", (unsigned long)(i + 1)])];
        icon.frame = CGRectMake(24 + col * (side + 24), 40 + row * (side + 36), side, side);
        icon.layer.cornerRadius = 18;
        icon.clipsToBounds = YES;
        [scroll addSubview:icon];
    }
    scroll.contentSize = CGSizeMake(self.view.bounds.size.width, 40 + 3 * (side + 36));
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    (void)application;
    (void)launchOptions;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];

    ProbeListViewController *list = [[ProbeListViewController alloc] initWithStyle:UITableViewStylePlain];
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:list];
    nav.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"RGBA"
                                                  image:probe_image(@"clock28x28.png") tag:0];

    TextFieldViewController *text = [[TextFieldViewController alloc] init];
    text.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Keyboard"
                                                   image:probe_image(@"clock28x65.png") tag:1];

    GridViewController *grid = [[GridViewController alloc] init];
    grid.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Folder"
                                                   image:probe_image(@"clock28x95.png") tag:2];

    UIViewController *thin = [[UIViewController alloc] init];
    thin.title = @"Thin";
    thin.view.backgroundColor = [UIColor systemBackgroundColor];
    thin.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Thin"
                                                   image:probe_image(@"clock6x85.png") tag:3];

    UITabBarController *tabs = [[UITabBarController alloc] init];
    tabs.viewControllers = @[nav, text, grid, thin];
    self.window.rootViewController = tabs;
    [self.window makeKeyAndVisible];
    return YES;
}

@end

int main(int argc, char *argv[])
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
