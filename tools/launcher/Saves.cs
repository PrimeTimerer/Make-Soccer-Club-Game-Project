// Save data page: the game's saves (PARAM.SFO titles and icons), backups and restoring one.
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace SakatsukuLauncher
{
    public class SavePage : PageBase
    {
        public SavePage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            List<SaveInfo> saves = SaveStore.List(Launcher);
            var list = new StackPanel();
            if (saves.Count == 0)
                list.Children.Add(Ui.Text("아직 세이브가 없습니다. 게임에서 저장하면 여기에 보입니다.", "Caption"));
            foreach (SaveInfo save in saves)
            {
                var row = new Grid { Margin = new Thickness(0, 0, 0, 12) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                ImageSource icon = Image(save.Icon);
                var picture = new Border
                {
                    Width = 160, Height = 88, CornerRadius = new CornerRadius(10), Margin = new Thickness(0, 0, 16, 0),
                    BorderBrush = Ui.Brush("Line"), BorderThickness = new Thickness(1),
                    Background = icon != null ? (Brush)new ImageBrush(icon) { Stretch = Stretch.UniformToFill } : Ui.Brush("Line"),
                };
                row.Children.Add(picture);
                var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                var title = new TextBlock { Text = save.Title.Length > 0 ? save.Title : save.Name, FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 15, FontWeight = FontWeights.SemiBold, Foreground = Ui.Brush("Text") };
                text.Children.Add(title);
                if (save.SubTitle.Length > 0 || save.Detail.Length > 0)
                    text.Children.Add(new TextBlock { Text = (save.SubTitle + " " + save.Detail).Trim(), FontFamily = Ui.Res<FontFamily>("TitleFont"), FontSize = 13, Foreground = Ui.Brush("SubText"), TextWrapping = TextWrapping.Wrap });
                text.Children.Add(Ui.Text(save.Name + " · " + save.Changed.ToString("yyyy-MM-dd HH:mm") + " · " + PcInfo.Size(save.Size), "Caption"));
                Grid.SetColumn(text, 1);
                row.Children.Add(text);
                list.Children.Add(row);
            }
            list.Children.Add(Ui.Row(
                Ui.Button("지금 백업", delegate
                {
                    try
                    {
                        string zip = SaveStore.Backup(Launcher, "manual");
                        Window.Toast(zip != null ? "백업했습니다: " + Path.GetFileName(zip) : "백업할 세이브가 없습니다");
                        Window.Refresh();
                    }
                    catch (Exception error)
                    {
                        Window.Error("백업하지 못했습니다", error);
                    }
                }, "AccentButton", "", new Thickness(0, 6, 8, 0)),
                Ui.Button("세이브 폴더 열기", delegate { Ui.OpenFolder(Launcher.SavesDir); }, null, "", new Thickness(0, 6, 0, 0))));

            List<string> backups = SaveStore.Backups(Launcher);
            var backupList = new StackPanel();
            if (backups.Count == 0)
                backupList.Children.Add(Ui.Text("아직 백업이 없습니다. 런처로 게임을 시작하면 그 전에 자동으로 만들어집니다 (설정 → 세이브 백업).", "Caption"));
            bool running = Launcher.Runner.Running || Game.IsRunning;
            foreach (string backup in backups)
            {
                string file = backup;
                var row = new Grid { Margin = new Thickness(0, 0, 0, 8) };
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var label = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                label.Children.Add(Ui.Text(SaveStore.Describe(file), "Body"));
                long size = 0;
                try
                {
                    size = new FileInfo(file).Length;
                }
                catch (Exception)
                {
                }
                label.Children.Add(Ui.Text(Path.GetFileName(file) + " · " + PcInfo.Size(size), "Caption"));
                row.Children.Add(label);
                var restore = Ui.Button("복원", delegate
                {
                    Window.Confirm("세이브 복원", SaveStore.Describe(file) + " 백업으로 세이브를 되돌립니다. 지금 세이브는 먼저 \"복원 전\" 백업으로 남깁니다.", "복원", delegate
                    {
                        try
                        {
                            SaveStore.Restore(Launcher, file);
                            Window.Toast("복원했습니다");
                            Window.Refresh();
                        }
                        catch (Exception error)
                        {
                            Window.Error("복원하지 못했습니다", error);
                        }
                    });
                }, null, "");
                restore.IsEnabled = !running;
                if (running)
                    restore.ToolTip = "게임을 끈 다음 복원할 수 있습니다.";
                Grid.SetColumn(restore, 1);
                row.Children.Add(restore);
                backupList.Children.Add(row);
            }
            backupList.Children.Add(Ui.Button("백업 폴더 열기", delegate { Ui.OpenFolder(Launcher.BackupsDir); }, null, "", new Thickness(0, 6, 0, 0)));
            ((FrameworkElement)backupList.Children[backupList.Children.Count - 1]).HorizontalAlignment = HorizontalAlignment.Left;

            return Ui.Page("세이브", "게임의 세이브(gamedata\\dev_hdd0\\home\\00000001\\savedata)와 백업. 백업은 세이브 전체를 zip 하나로 남깁니다.",
                Ui.Card("세이브 데이터", list),
                Ui.Card("백업", backupList));
        }
    }
}
