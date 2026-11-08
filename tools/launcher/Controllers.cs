// Controllers page: the XInput pads the game sees (it numbers them as XInput does) and the port's fixed button map
// (ps3recomp/libs/input/cellPad.c), with the keyboard pad it gives 1P when no pad is connected.
using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace SakatsukuLauncher
{
    static class XInput
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct State
        {
            public uint PacketNumber;
            public ushort Buttons;
            public byte LeftTrigger, RightTrigger;
            public short ThumbLX, ThumbLY, ThumbRX, ThumbRY;
        }

        [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
        static extern uint GetState14(uint index, out State state);

        [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
        static extern uint GetState910(uint index, out State state);

        static int dll; // 0 untried, 1 xinput1_4, 2 xinput9_1_0, -1 none

        public static bool Connected(uint index)
        {
            State state;
            if (dll >= 0 && dll != 2)
            {
                try
                {
                    bool ok = GetState14(index, out state) == 0;
                    dll = 1;
                    return ok;
                }
                catch (Exception)
                {
                    dll = 2;
                }
            }
            if (dll == 2)
            {
                try
                {
                    return GetState910(index, out state) == 0;
                }
                catch (Exception)
                {
                    dll = -1;
                }
            }
            return false;
        }
    }

    public class ControllerPage : PageBase
    {
        DispatcherTimer poll;
        string seen = "";

        public ControllerPage(MainWindow window) : base(window)
        {
        }

        static string Connections()
        {
            string s = "";
            for (uint i = 0; i < 4; i++)
                s += XInput.Connected(i) ? "1" : "0";
            return s;
        }

        public override void Shown()
        {
            seen = Connections();
            base.Shown();
            if (poll == null)
            {
                poll = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
                poll.Tick += delegate
                {
                    string now = Connections();
                    if (now != seen)
                    {
                        seen = now;
                        base.Shown();
                    }
                };
            }
            poll.Start();
        }

        public override void Hidden()
        {
            if (poll != null)
                poll.Stop();
        }

        protected override FrameworkElement Build()
        {
            var pads = new StackPanel();
            for (int i = 0; i < 4; i++)
            {
                bool on = seen.Length == 4 && seen[i] == '1';
                pads.Children.Add(Line((i + 1) + "P", on ? "연결됨 (XInput 패드 " + (i + 1) + ")" : "없음" + (i == 0 && Launcher.On("Keyboard") ? " · 키보드로 조작" : ""), on ? Ui.Brush("Good") : null));
            }
            pads.Children.Add(Ui.Text("Xbox 호환 패드(XInput)를 씁니다. DualShock·DualSense는 Steam 입력이나 DS4Windows로 XInput 패드로 바꿔 연결하세요.", "Caption"));

            var map = new Grid();
            map.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(150) });
            map.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(190) });
            map.ColumnDefinitions.Add(new ColumnDefinition());
            string[,] rows =
            {
                { "PS3", "패드 (XInput)", "키보드 (1P, 패드 없을 때)" },
                { "×", "A", "Z" }, { "○", "B", "X" }, { "□", "X", "A" }, { "△", "Y", "S" },
                { "L1 / R1", "LB / RB", "Q / W" }, { "L2 / R2", "LT / RT", "1 / 2" }, { "L3 / R3", "스틱 누르기", "—" },
                { "START", "Start (☰)", "Enter" }, { "SELECT", "Back (⧉)", "Tab" },
                { "방향 버튼", "방향 패드", "방향키" }, { "왼쪽 스틱", "왼쪽 스틱", "방향키 (같이 움직임)" }, { "오른쪽 스틱", "오른쪽 스틱", "—" },
            };
            for (int r = 0; r < rows.GetLength(0); r++)
            {
                map.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                for (int c = 0; c < 3; c++)
                {
                    var cell = Ui.Text(rows[r, c], r == 0 ? "Caption" : "Body");
                    cell.Margin = new Thickness(0, 0, 0, 8);
                    if (r > 0 && c == 0)
                        cell.FontWeight = FontWeights.SemiBold;
                    Grid.SetRow(cell, r);
                    Grid.SetColumn(cell, c);
                    map.Children.Add(cell);
                }
            }

            return Ui.Page("컨트롤러", "버튼 배치는 포트에 정해져 있습니다. 패드를 꽂거나 빼면 바로 바뀝니다.",
                Ui.Card("연결된 패드", pads),
                Ui.Card("버튼 배치", map,
                    Ui.Text("키보드는 1P 패드가 없을 때만, 게임 창이 앞에 있을 때만 쓰입니다. 끄려면 설정 → 키보드로 조작.", "Caption")));
        }
    }
}
