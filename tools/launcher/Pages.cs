// Home and settings pages.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;

namespace SakatsukuLauncher
{
    // A page built again each time it is shown, so it always shows the files as they are.
    public abstract class PageBase : IPage
    {
        protected readonly MainWindow Window;
        readonly ContentControl view = new ContentControl();

        protected PageBase(MainWindow window)
        {
            Window = window;
        }

        protected Launcher Launcher { get { return Window.Launcher; } }
        public FrameworkElement View { get { return view; } }

        public virtual void Shown()
        {
            view.Content = Build();
        }

        public virtual void Hidden()
        {
        }

        protected abstract FrameworkElement Build();

        public static ImageSource Image(string path)
        {
            if (path == null || !File.Exists(path))
                return null;
            try
            {
                var image = new BitmapImage();
                image.BeginInit();
                image.CacheOption = BitmapCacheOption.OnLoad; // the file is not kept open
                image.UriSource = new Uri(path);
                image.EndInit();
                return image;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static FrameworkElement Line(string label, string value, Brush valueBrush = null)
        {
            var grid = new Grid { Margin = new Thickness(0, 0, 0, 8) };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.Children.Add(Ui.Text(label, "Caption"));
            var text = Ui.Text(value, "Body");
            text.TextWrapping = TextWrapping.Wrap;
            if (valueBrush != null)
                text.Foreground = valueBrush;
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            return grid;
        }
    }

    public class HomePage : PageBase
    {
        public HomePage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            GameFiles files = GameFiles.Check(Launcher);
            var stack = new StackPanel { Margin = new Thickness(16, 8, 16, 24) };
            stack.Children.Add(Hero(files));
            Border filesCard = FilesCard(files), record = RecordCard(), pcCard = PcCard(PcInfo.Read());
            filesCard.Margin = record.Margin = new Thickness(0, 0, 16, 16);
            stack.Children.Add(Ui.Columns(3, filesCard, record, pcCard));
            return new ScrollViewer { Content = stack, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        }

        FrameworkElement Hero(GameFiles files)
        {
            var hero = new Grid { Height = 370, Margin = new Thickness(0, 0, 0, 18) };
            hero.SizeChanged += delegate { hero.Clip = new RectangleGeometry(new Rect(0, 0, hero.ActualWidth, hero.ActualHeight), 24, 24); };
            var gradient = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 1) };
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x0B, 0x24, 0x22), 0));
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x0E, 0x16, 0x36), 0.62));
            gradient.GradientStops.Add(new GradientStop(Color.FromRgb(0x0A, 0x12, 0x24), 1));
            hero.Children.Add(new Border { Background = gradient, CornerRadius = new CornerRadius(24), BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1) });
            // The disc's own background picture (PIC1.PNG), dimmed under a gradient that keeps the text readable.
            ImageSource pic = Image(files.Cover(Launcher, "PIC1.PNG"));
            if (pic != null)
            {
                hero.Children.Add(new Border { Background = new ImageBrush(pic) { Stretch = Stretch.UniformToFill, Opacity = 0.38 }, CornerRadius = new CornerRadius(24) });
                var shade = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
                shade.GradientStops.Add(new GradientStop(Color.FromArgb(0xF0, 0x07, 0x0B, 0x14), 0));
                shade.GradientStops.Add(new GradientStop(Color.FromArgb(0xA0, 0x07, 0x0B, 0x14), 0.55));
                shade.GradientStops.Add(new GradientStop(Color.FromArgb(0x30, 0x07, 0x0B, 0x14), 1));
                hero.Children.Add(new Border { Background = shade, CornerRadius = new CornerRadius(24) });
            }
            FrameworkElement art = PitchArt();
            art.HorizontalAlignment = HorizontalAlignment.Right;
            art.VerticalAlignment = VerticalAlignment.Bottom;
            art.Margin = new Thickness(0, 0, 18, 14);
            hero.Children.Add(art);

            string custom = Launcher.State("CoverImage", "");
            ImageSource cover = Image(custom.Length > 0 ? custom : files.Cover(Launcher, "ICON0.PNG"));
            if (cover != null)
            {
                // The disc icon is wide (320x176): top right, clear of the title and the chips; a portrait cover beside them.
                bool wide = cover.Width > cover.Height;
                hero.Children.Add(new Border
                {
                    Width = wide ? 256 : 200, Height = wide ? 141 : 272, CornerRadius = new CornerRadius(16), HorizontalAlignment = HorizontalAlignment.Right,
                    VerticalAlignment = wide ? VerticalAlignment.Top : VerticalAlignment.Center, Margin = new Thickness(0, wide ? 64 : 0, 40, 0),
                    Background = new ImageBrush(cover) { Stretch = Stretch.UniformToFill }, BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1),
                    Effect = new DropShadowEffect { Color = Color.FromRgb(0x22, 0xD3, 0xEE), BlurRadius = 36, ShadowDepth = 0, Opacity = 0.45 },
                });
            }

            var left = new StackPanel { Margin = new Thickness(44, 30, 40, 30), VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Left };
            var caption = Ui.Text("PLAYSTATION 3  ·  SEGA  ·  " + GameFiles.TitleId + (files.Patch ? " " + files.PatchVersion : ""), "Caption");
            caption.Foreground = Ui.Brush("Accent");
            caption.FontWeight = FontWeights.SemiBold;
            left.Children.Add(caption);
            int space = files.Title.IndexOf(' ');
            left.Children.Add(new TextBlock
            {
                Text = space > 0 ? files.Title.Substring(0, space) + "\n" + files.Title.Substring(space + 1) : files.Title,
                FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 34, FontWeight = FontWeights.Bold,
                Foreground = Brushes.White, LineHeight = 44, Margin = new Thickness(0, 10, 0, 0),
                Effect = new DropShadowEffect { Color = Color.FromRgb(0x10, 0xB9, 0x81), BlurRadius = 26, ShadowDepth = 0, Opacity = 0.7 },
            });
            bool fps60 = Launcher.On("Fps60");
            var subtitle = Ui.Text("PC 포트  ·  " + (fps60 ? "60fps · 원래 게임 속도" : "30fps (원본과 같음)"), "Caption");
            subtitle.FontSize = 14;
            subtitle.Margin = new Thickness(0, 10, 0, 18);
            left.Children.Add(subtitle);

            var chips = new WrapPanel { MaxWidth = 560, Margin = new Thickness(0, 0, 0, 18) };
            chips.Children.Add(files.Ready ? Ui.Chip("", "게임 파일 확인됨", Ui.Brush("Good")) : Ui.Chip("", "게임 파일 부족", Ui.Brush("Warn")));
            chips.Children.Add(Ui.Chip("", fps60 ? "60fps 모드" : "60fps 모드 꺼짐", fps60 ? Ui.Brush("Accent") : Ui.Brush("SubText")));
            int saves = SaveStore.List(Launcher).Count;
            if (saves > 0)
                chips.Children.Add(Ui.Chip("", "세이브 " + saves + "개", Ui.Brush("Accent")));
            int cheats = Cheats.EnabledCount(Launcher);
            if (cheats > 0)
                chips.Children.Add(Ui.Chip("", "치트 " + cheats + "개 켜짐", Ui.Brush("Accent2")));
            if (Launcher.On("AutoBackup"))
                chips.Children.Add(Ui.Chip("", "시작 전 자동 백업", Ui.Brush("SubText")));
            left.Children.Add(chips);

            var play = new Button { Style = Ui.Style("PlayButton"), Content = Ui.Row(Ui.Icon("", 20, Brushes.White), new TextBlock { Text = "  플레이", VerticalAlignment = VerticalAlignment.Center }) };
            play.Click += delegate { Window.Play(false); };
            var safe = Ui.Button("안전 설정으로 실행", delegate { Window.Play(true); }, null, "", new Thickness(16, 0, 0, 0));
            safe.ToolTip = "이번 한 번만 원래 30fps, 정점 캐시·치트 끔으로 실행합니다 (설정은 바뀌지 않습니다).";
            var buttons = Ui.Row(play, safe);
            buttons.VerticalAlignment = VerticalAlignment.Center;
            left.Children.Add(buttons);
            var keys = Ui.Text("패드가 없으면 키보드: 방향키 · Z × · X ○ · A □ · S △ · Enter START", "Caption");
            keys.Margin = new Thickness(4, 14, 0, 0);
            left.Children.Add(keys);
            hero.Children.Add(left);

            var coverButton = Ui.Button("표지 이미지", PickCover, null, "");
            coverButton.HorizontalAlignment = HorizontalAlignment.Right;
            coverButton.VerticalAlignment = VerticalAlignment.Top;
            coverButton.Margin = new Thickness(0, 16, 16, 0);
            coverButton.Padding = new Thickness(12, 6, 12, 6);
            coverButton.Opacity = 0.85;
            hero.Children.Add(coverButton);
            return hero;
        }

        // A football pitch in neon lines.
        static FrameworkElement PitchArt()
        {
            var canvas = new Canvas { Width = 520, Height = 300, Opacity = 0.42, IsHitTestVisible = false };
            var line = new SolidColorBrush(Color.FromRgb(0x34, 0xD3, 0x99));
            var cyan = new SolidColorBrush(Color.FromRgb(0x22, 0xD3, 0xEE));
            Action<string, Brush, double> add = delegate(string data, Brush brush, double thickness)
            {
                canvas.Children.Add(new System.Windows.Shapes.Path { Data = Geometry.Parse(data), Stroke = brush, StrokeThickness = thickness, StrokeLineJoin = PenLineJoin.Round });
            };
            // In perspective: the far touchline is shorter than the near one.
            add("M 110,40 L 410,40 L 510,290 L 10,290 Z", line, 2.0);                                   // touchlines, goal lines
            add("M 260,40 L 260,290", line, 1.6);                                                       // halfway line
            add("M 72,132 L 128,132 L 112,226 L 40,226 Z M 448,132 L 392,132 L 408,226 L 480,226 Z", cyan, 1.6); // penalty areas
            add("M 92,158 L 106,158 L 100,198 L 82,198 Z M 428,158 L 414,158 L 420,198 L 438,198 Z", cyan, 1.2); // goal areas
            var circle = new Ellipse { Width = 96, Height = 46, Stroke = line, StrokeThickness = 1.6 };
            Canvas.SetLeft(circle, 212);
            Canvas.SetTop(circle, 140);
            canvas.Children.Add(circle);
            var spot = new Ellipse { Width = 6, Height = 4, Fill = line };
            Canvas.SetLeft(spot, 257);
            Canvas.SetTop(spot, 161);
            canvas.Children.Add(spot);
            canvas.Effect = new DropShadowEffect { Color = Color.FromRgb(0x10, 0xB9, 0x81), BlurRadius = 14, ShadowDepth = 0, Opacity = 1 };
            return canvas;
        }

        void PickCover()
        {
            Action pick = delegate
            {
                var dialog = new Microsoft.Win32.OpenFileDialog { Title = "표지 이미지", Filter = "이미지 (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp" };
                if (dialog.ShowDialog(Window) != true)
                    return;
                try
                {
                    string target = System.IO.Path.Combine(Launcher.Dir, "launcher-cover" + System.IO.Path.GetExtension(dialog.FileName).ToLowerInvariant());
                    File.Copy(dialog.FileName, target, true);
                    Launcher.SetState("CoverImage", target);
                    Window.Refresh();
                }
                catch (Exception error)
                {
                    Window.Error("표지 이미지를 쓰지 못했습니다", error);
                }
            };
            if (Launcher.State("CoverImage", "").Length == 0)
            {
                Window.Dialog("", Ui.Brush("Accent"), "표지 이미지", "지금은 디스크의 아이콘(ICON0.PNG)을 씁니다. 직접 가진 표지 스캔이나 좋아하는 그림을 고를 수 있습니다 (Steam 바로가기 그림에도 씁니다).",
                              new DialogAction("이미지 고르기", pick, true), new DialogAction("취소", null));
                return;
            }
            Window.Dialog("", Ui.Brush("Accent"), "표지 이미지", "홈과 Steam 바로가기에 쓰는 그림입니다.",
                          new DialogAction("다른 이미지 고르기", pick, true),
                          new DialogAction("디스크 아이콘으로", delegate
                          {
                              Launcher.SetState("CoverImage", "");
                              Window.Refresh();
                          }),
                          new DialogAction("취소", null));
        }

        Border FilesCard(GameFiles files)
        {
            var stack = new StackPanel();
            Func<bool, string, string, FrameworkElement> item = delegate(bool ok, string label, string value)
            {
                return Line(label, (ok ? "✓ " : "✕ ") + value, ok ? Ui.Brush("Good") : Ui.Brush("Warn"));
            };
            stack.Children.Add(item(files.Eboot, "EBOOT.ELF", files.Eboot ? "1.03 ELF" : "없음 (1.03 ELF 필요)"));
            stack.Children.Add(item(files.Disc, "디스크", files.Disc ? GameFiles.TitleId + " " + files.DiscVersion : "PS3_GAME 폴더 없음"));
            stack.Children.Add(item(files.Patch, "패치", files.Patch ? files.PatchVersion : files.PatchVersion.Length > 0 ? files.PatchVersion + " (1.03 필요)" : "1.03 패치 없음"));
            bool exe = File.Exists(Launcher.Exe);
            stack.Children.Add(item(exe, "실행 파일", exe ? (Launcher.Exe.Contains("build-release") ? "릴리스 빌드" : "디버그 빌드") : "빌드 필요 (build_release.bat)"));
            var open = Ui.Button("game 폴더 열기", delegate { Ui.OpenFolder(Launcher.GameDir); }, null, "", new Thickness(0, 6, 0, 0));
            open.HorizontalAlignment = HorizontalAlignment.Left;
            stack.Children.Add(open);
            return Ui.Card("게임 파일", stack);
        }

        Border RecordCard()
        {
            long seconds;
            long.TryParse(Launcher.State("PlayTimeSeconds", "0"), out seconds);
            string last = Launcher.State("LastPlayed", "");
            var stack = new StackPanel();
            stack.Children.Add(Line("마지막 플레이", last.Length > 0 ? last : "아직 없음"));
            stack.Children.Add(Line("총 플레이 시간", seconds / 3600 + "시간 " + seconds / 60 % 60 + "분"));
            LogInfo run = Logs.List(Launcher.LogsDir).FirstOrDefault();
            if (run != null)
            {
                bool ok = run.ExitCode == "0";
                stack.Children.Add(Line("최근 실행", run.ExitCode == null ? "기록 없음" : ok ? "정상 종료" : "오류로 끝남 (코드 " + run.ExitCode + ")", ok ? null : Ui.Brush("Bad")));
                if (!ok && run.ExitCode != null)
                {
                    var logs = Ui.Button("로그 보기", delegate { Window.Navigate("logs"); }, null, "", new Thickness(0, 6, 0, 0));
                    logs.HorizontalAlignment = HorizontalAlignment.Left;
                    stack.Children.Add(logs);
                }
            }
            return Ui.Card("플레이 기록", stack);
        }

        Border PcCard(PcInfo pc)
        {
            var stack = new StackPanel();
            stack.Children.Add(Line("CPU", pc.Cpu + " · " + pc.Threads + "스레드"));
            stack.Children.Add(Line("그래픽카드", pc.Gpu + " (" + PcInfo.Size(pc.Vram) + ")"));
            stack.Children.Add(Line("메모리", PcInfo.Size(pc.Ram)));
            stack.Children.Add(Line("모니터", pc.ScreenWidth + "×" + pc.ScreenHeight));
            return Ui.Card("이 PC", stack);
        }
    }

    public class SettingsPage : PageBase
    {
        public SettingsPage(MainWindow window) : base(window)
        {
        }

        void Set(string key, string value)
        {
            Launcher.SetSetting(key, value);
            Window.Toast("저장했습니다");
        }

        UIElement Toggle(string key)
        {
            return Ui.Switch(Launcher.On(key), delegate(bool on)
            {
                Set(key, on ? "1" : "0");
                Window.Refresh(); // the settings that depend on it
            });
        }

        UIElement Pick(string key, params string[] valueLabelPairs)
        {
            return Ui.Choices(Launcher.Setting(key), delegate(string v) { Set(key, v); }, valueLabelPairs);
        }

        // The Korean patch beside the game files (KoreanPatch.cs): install from the patch exe, switch, remove.
        FrameworkElement KoreanCard()
        {
            bool installed = KoreanPatch.Installed(Launcher);
            var buttons = new List<UIElement> { Ui.Button(installed ? "다시 설치" : "설치", InstallKorean) };
            if (installed)
                buttons.Add(Ui.Button("삭제", delegate
                {
                    Window.Confirm("한글패치 삭제", "game\\mods\\korean 폴더를 지웁니다. 원본 게임 파일은 처음부터 그대로입니다.", "삭제", delegate
                    {
                        KoreanPatch.Remove(Launcher);
                        Launcher.SetSetting("KoreanPatch", "0");
                        Window.Toast("한글패치를 지웠습니다");
                        Window.Refresh();
                    });
                }));
            return Ui.Card("한글패치",
                Dim(Ui.Setting("한글패치 사용", installed ? KoreanPatch.Info(Launcher) + ". 원본 파일은 그대로 두고 이 한글 파일을 먼저 읽습니다. 끄면 다음 실행부터 일본어 원본입니다." : "먼저 아래에서 한글패치를 설치하세요.", Toggle("KoreanPatch")), installed),
                Ui.Setting("한글패치 설치", "한글패치 exe를 고르면 실행하지 않고 안의 데이터만 읽어 game\\mods\\korean 폴더에 한글 파일을 만듭니다. 게임 원본 파일은 바꾸지 않습니다.", Ui.Row(buttons.ToArray())));
        }

        void InstallKorean()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Title = "한글패치 파일 (Sakatsuku_Korean_Patch_*.exe)", Filter = "한글패치 (*.exe)|*.exe" };
            if (dialog.ShowDialog(Window) != true)
                return;
            try
            {
                System.Windows.Input.Mouse.OverrideCursor = System.Windows.Input.Cursors.Wait;
                string report = KoreanPatch.Install(Launcher, dialog.FileName);
                Launcher.SetSetting("KoreanPatch", "1");
                Window.Toast(report + " 설치, 다음 실행부터 한글");
            }
            catch (Exception error)
            {
                Window.Error("한글패치를 설치하지 못했습니다", error);
            }
            finally
            {
                System.Windows.Input.Mouse.OverrideCursor = null;
                Window.Refresh();
            }
        }

        static FrameworkElement Dim(FrameworkElement setting, bool enabled)
        {
            setting.IsEnabled = enabled;
            setting.Opacity = enabled ? 1 : 0.45;
            return setting;
        }

        protected override FrameworkElement Build()
        {
            bool fps60 = Launcher.On("Fps60"), cache = Launcher.On("VertexCache");
            int keep;
            if (!int.TryParse(Launcher.Setting("BackupsToKeep"), out keep))
                keep = 10;
            var env = new StackPanel();
            foreach (KeyValuePair<string, string> pair in Settings.Environment(Launcher, false).Where(p => p.Value != null))
                env.Children.Add(new TextBlock { Text = pair.Key + "=" + pair.Value, FontFamily = Ui.Res<FontFamily>("MonoFont"), FontSize = 12, Foreground = Ui.Brush("SubText") });

            return Ui.Page("설정", "바꾸면 바로 launcher.ini에 저장되고, 다음에 게임을 시작할 때 적용됩니다.",
                Ui.Card("60fps",
                    Ui.Setting("60fps 모드", "게임을 1초 60틱으로 돌립니다. 경기 계산은 30Hz 그대로라 결과는 원본과 같고, 게임 속도도 원래 속도입니다.", Toggle("Fps60")),
                    Dim(Ui.Setting("선수 움직임 60Hz", "경기 계산이 없는 틱에 선수 애니메이션을 반 스텝 앞당겨 그립니다. 경기 결과는 바뀌지 않습니다.", Toggle("PlayerMotion60")), fps60),
                    Dim(Ui.Setting("공 움직임 60Hz", "같은 틱에 공이 그려지는 위치만 반 스텝 앞당깁니다 (공 자체는 그대로). 차기·튕김·멈춤은 그대로 둡니다.", Toggle("BallMotion60")), fps60),
                    Ui.Setting("공 순간이동 부드럽게", "착지 뒤나 드리블 중에 공이 잠깐 멈췄다가 순간이동하는 장면을 화면에서만 미끄러지듯 이어서 그립니다. 차는 순간과 재시작은 그대로 두고, 경기 계산은 건드리지 않습니다. 30fps 모드에서도 적용됩니다.", Toggle("BallHopSmooth"))),
                Ui.Card("렌더러",
                    Ui.Setting("정점 캐시", "같은 정점 데이터를 다시 변환하지 않아 무거운 장면의 프레임이 오릅니다.", Toggle("VertexCache")),
                    Dim(Ui.Setting("정점 캐시 용량", "PC 메모리에서 쓰는 최대 크기입니다.", Pick("VertexCacheMB", "256", "256 MB", "512", "512 MB", "1024", "1 GB (기본)", "2048", "2 GB")), cache),
                    Ui.Setting("수직 동기화 (VSync)", "끄면 화면이 찢어져 보일 수 있습니다. 게임 속도는 바뀌지 않습니다.", Toggle("VSync")),
                    Ui.Setting("고주사율 모니터 프레임 간격 맞춤", "120/180/240Hz 모니터에서 프레임마다 같은 시간(16.7ms) 보이게 합니다. 고르게 보이지만, 무거운 장면에서 늦은 프레임은 33ms 보입니다. 직접 비교해 보고 정하세요.", Toggle("SyncHighRefresh"))),
                KoreanCard(),
                Ui.Card("소리와 조작",
                    Ui.Setting("소리 끄기", null, Toggle("Mute")),
                    Ui.Setting("키보드로 조작", "패드가 연결되지 않았을 때 1P를 키보드로 조작합니다 (배치는 컨트롤러 페이지).", Toggle("Keyboard"))),
                Ui.Card("세이브 백업",
                    Ui.Setting("실행 전 자동 백업", "게임을 시작할 때마다 세이브 전체를 gamedata\\backups에 zip으로 남깁니다.", Toggle("AutoBackup")),
                    Ui.Setting("보관할 백업 수", "넘으면 가장 오래된 것부터 지웁니다.", Ui.Slider(3, 30, keep, 1, delegate(double v) { return ((int)v) + "개"; }, delegate(double v) { Set("BackupsToKeep", ((int)v).ToString()); }))),
                Ui.Card("게임에 넘기는 값",
                    Ui.Text("포트에는 설정 파일이 없어서, 런처가 게임을 시작할 때 아래 환경변수로 넘깁니다 (run.bat와 같은 방식).", "Caption"),
                    env,
                    Ui.Row(Ui.Button("기본값으로 되돌리기", delegate
                    {
                        Window.Confirm("설정 되돌리기", "모든 설정을 기본값(60fps 모드, 선수·공 60Hz, 공 순간이동 보간, 정점 캐시 1 GB, VSync, 자동 백업)으로 되돌립니다.", "되돌리기", delegate
                        {
                            foreach (string key in Settings.Keys)
                                Launcher.SetSetting(key, Settings.Default(key));
                            Window.Toast("기본값으로 되돌렸습니다");
                            Window.Refresh();
                        });
                    }, null, "", new Thickness(0, 14, 8, 0)), Ui.Button("launcher.ini 열기", delegate
                    {
                        if (!File.Exists(Launcher.StatePath))
                            Launcher.SetSetting("Fps60", Launcher.Setting("Fps60"));
                        System.Diagnostics.Process.Start("notepad.exe", "\"" + Launcher.StatePath + "\"");
                    }, null, "", new Thickness(0, 14, 0, 0)))));
        }
    }
}
