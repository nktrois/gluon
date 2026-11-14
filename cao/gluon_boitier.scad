/* ============================================================
   GLUON — boîtier v4
   - trou du bouton à 22, dessus plat
   - écran fixé par 4 vis M2 sur colonnettes (entraxe 40 x 32,5)
   - haut-parleur maintenu par une portée à épaulement
   - volume interne augmenté : +10 mm de profondeur, +5 de hauteur
   ============================================================ */

// --- Écran (Waveshare 1.5" RGB OLED) ---
aff_l    = 30;   aff_h   = 34;    // zone allumée = la fenêtre
pcb_l    = 44;   pcb_h   = 36;    // carte
vis_x    = 40;   vis_y   = 32.5;  // entraxe des 4 trous de fixation
verre_l  = 35;   verre_h = 35;    // carré de verre, cache derriere la facade
verre_ep = 2.0;  pcb_ep  = 1.7;   // <-- verre_ep A MESURER : epaisseur du verre seul
laiton_l = 6;                     // profondeur du logement des entretoises laiton

// --- Bouton d'arcade ---
btn_d    = 22;                    // perçage traversant
btn_bar  = 36;                    // dégagement de l'écrou
btn_prof = 47;                    // profondeur sous la collerette

// --- Haut-parleur ---
hp_d     = 11.4;  hp_ep = 8;      // le HP se colle dans un berceau annulaire

// --- Boîtier ---
paroi    = 2.4;   jeu = 0.35;
W        = 68;    D = 66;   H = 75;   r_coin = 2;   // profondeur augmentee
cadre    = 2.0;
b_haut   = 4;     b_cote = 4;   b_bas = 16;

// --- Positions ---
ecran_z  = (b_bas + (H - b_haut)) / 2;
btn_y    = D / 2;   // centre pile dans la profondeur
hp_z     = 9.5;
usb_cote = 1;     usb_y = 36;   usb_w = 17;   usb_h = 12;
socle_ep = 3;     retrait = 2;   lip_h = 3;   lip_w = 2;

// dos du PCB : c'est la que viennent buter les colonnettes
y_col = paroi + verre_ep + pcb_ep;

piece = "coque";  // "coque" | "socle"
$fn = 48;

// ============================================================
module boite(l, p, h, r) {
  hull() for (x = [-l/2 + r, l/2 - r], y = [r, p - r])
    translate([x, y, 0]) cylinder(r = r, h = h);
}

module panneau(dy, ep) {
  translate([-(W/2 - b_cote), dy, b_bas])
    cube([W - 2 * b_cote, ep, H - b_haut - b_bas]);
}

// La dalle de verre TRAVERSE la façade et affleure la surface.
// Le PCB, lui, s'appuie a plat contre l'interieur de la paroi.
module fenetre() {
  hull() {
    // le petit cote doit etre PILE au nu exterieur, sinon l'ouverture
    // visible est deja evasee et on apercoit le PCB
    translate([0, -0.1, ecran_z]) cube([aff_l, 0.2, aff_h], center = true);
    translate([0, paroi, ecran_z])
      cube([aff_l + 2*paroi, 0.2, aff_h + 2*paroi], center = true);
  }
}


// Degagement pour le connecteur blanc et la nappe, cote interieur.
// Le verre se loge entre la facade et le PCB. Rien ne doit le gener.
module degagement_module() {
  translate([0, paroi - 0.01, ecran_z])
    cube([verre_l + 2*jeu, verre_ep + 0.02, verre_h + 2*jeu], center = true);
}

module pos_vis_ecran() {
  for (x = [-vis_x/2, vis_x/2], z = [ecran_z - vis_y/2, ecran_z + vis_y/2])
    translate([x, 0, z]) children();
}

// Fentes laterales seulement : le centre est reserve aux trous de son,
// sinon les deux geometries se touchent et cassent le maillage.
module grille() {
  for (z = [5, 9.5, 14], sx = [-16, 7])
    translate([sx, -cadre - 1, z - 1.3]) cube([9, paroi + cadre + 3, 2.6]);
}

// Trous de sortie du son, juste devant la membrane du HP.
module logement_hp() {
  // deux couronnes decalees de 30 degres : sinon les trous se touchent
  for (a = [0 : 60 : 359])
    translate([cos(a) * 2.6, -cadre - 1, hp_z + sin(a) * 2.6])
      rotate([-90, 0, 0]) cylinder(d = 1.8, h = paroi + cadre + 3);
  for (a = [30 : 60 : 359])
    translate([cos(a) * 4.6, -cadre - 1, hp_z + sin(a) * 4.6])
      rotate([-90, 0, 0]) cylinder(d = 1.8, h = paroi + cadre + 3);
  translate([0, -cadre - 1, hp_z]) rotate([-90, 0, 0]) cylinder(d = 1.8, h = paroi + cadre + 3);
}

module deco_arriere() {
  for (z = [5, 9.5, 14])
    translate([0, D - 0.6, z]) cube([30, 1.2, 2.6], center = true);
}
module deco_cotes() {
  for (x = [-W/2, W/2], z = [5, 9.5, 14], seg = [[5, 16], [42, 54]])
    translate([x, (seg[0]+seg[1])/2, z]) cube([2, seg[1]-seg[0], 2.6], center = true);
}

module passage_usb() {
  x0 = usb_cote > 0 ? W/2 - paroi - 3 : -W/2 - 5;
  translate([x0, usb_y - usb_w/2, -socle_ep - 2])
    cube([paroi + 8, usb_w, usb_h + socle_ep + 2]);
}

cvx = W/2 - paroi - 4.5;   cvy1 = paroi + 4.5;   cvy2 = D - paroi - 4.5;
module pos_vis() { for (x = [-cvx, cvx], y = [cvy1, cvy2]) translate([x, y, 0]) children(); }

// ============================================================
module coque() {
  difference() {
    union() {
      boite(W, D, H, r_coin);
      difference() {
        translate([-W/2, -cadre, 0]) cube([W, cadre + 0.01, H]);
        panneau(-cadre - 0.5, cadre + 0.5);
      }
    }

    translate([0, paroi, -1])
      boite(W - 2*paroi, D - 2*paroi, H - paroi + 1, max(r_coin - paroi, 0.1));

    fenetre();
    degagement_module();
    grille();
    logement_hp();
    deco_arriere();
    deco_cotes();
    passage_usb();

    // bouton : simple perçage, dessus plat
    translate([0, btn_y, H - paroi - 1]) cylinder(d = btn_d, h = paroi + 3);
    // le degagement de l'ecrou doit s'arreter SOUS la paroi du dessus,
    // sinon c'est lui qu'on voit et le trou fait 36 au lieu de btn_d
    translate([0, btn_y, H - btn_prof]) cylinder(d = btn_bar, h = btn_prof - paroi);

    pos_vis() translate([0, 0, -1]) cylinder(d = 2.6, h = 14);
  }

  // ---- Colonnettes de fixation de l'écran ----
  // Face avant au niveau du DOS du PCB : en serrant, la vis tire le
  // laiton vers l'arriere et plaque le PCB contre cette face, ce qui
  // maintient le verre contre la facade.
  // Chaque colonnette est tenue par une nervure qui rejoint la paroi,
  // en passant derriere le PCB pour ne pas le gener.
  difference() {
    union() {
      pos_vis_ecran() translate([0, y_col, 0]) rotate([-90, 0, 0])
        cylinder(d = 9, h = laiton_l + 4);
      // nervures vers les parois laterales
      for (sgn = [-1, 1], z = [ecran_z - vis_y/2, ecran_z + vis_y/2])
        translate([sgn * (vis_x/2 + (W/2 - paroi - vis_x/2) / 2), y_col + (laiton_l + 4)/2, z])
          cube([W/2 - paroi - vis_x/2 + 1, laiton_l + 4, 2.6], center = true);
    }
    // logement du laiton, puis passage de la vis M2
    pos_vis_ecran() translate([0, y_col - 0.1, 0]) rotate([-90, 0, 0])
      cylinder(d = 5.4, h = laiton_l + 0.1);
    pos_vis_ecran() translate([0, y_col - 1, 0]) rotate([-90, 0, 0])
      cylinder(d = 2.4, h = laiton_l + 8);
  }

  // Berceau du haut-parleur : une couronne contre laquelle il se colle,
  // pile derriere la grille.
  difference() {
    // la couronne mord de 0,6 mm dans la paroi pour fusionner proprement
    translate([0, paroi - 0.6, hp_z]) rotate([-90, 0, 0])
      cylinder(d = hp_d + 4.5, h = hp_ep - 0.4);
    translate([0, paroi - 1.6, hp_z]) rotate([-90, 0, 0])
      cylinder(d = hp_d + 0.5, h = hp_ep + 3);
  }

  intersection() {
    difference() {
      boite(W - 2*paroi, D - 2*paroi, 13, max(r_coin - paroi, 0.1));
      pos_vis() translate([0, 0, -1]) cylinder(d = 2.6, h = 16);
    }
    pos_vis() cylinder(d = 10, h = 13);
  }
}

// ============================================================
module socle() {
  difference() {
    union() {
      boite(W - 2*retrait, D - 2*retrait, socle_ep, r_coin);
      translate([0, paroi + jeu - retrait, socle_ep - 0.6])
        difference() {
          boite(W - 2*(paroi+jeu), D - 2*(paroi+jeu), lip_h + 0.6, 0.8);
          translate([0, lip_w, -1])
            boite(W - 2*(paroi+jeu+lip_w), D - 2*(paroi+jeu+lip_w), lip_h + 3, 0.8);
        }
    }
    translate([0, -retrait, 0]) {
      pos_vis() translate([0, 0, -1]) {
        cylinder(d = 3.3, h = 14);
        cylinder(d1 = 6.6, d2 = 3.3, h = 2.8);
      }
      pos_vis() translate([0, 0, socle_ep - 0.01]) cylinder(d = 10 + 2*jeu, h = 6);
    }
    for (x = [-16 : 8 : 16], y = [14 : 7 : 44])
      translate([x, y - retrait, -1]) cylinder(d = 3.4, h = socle_ep + 2);
    translate([0, -retrait, socle_ep]) passage_usb();
  }
}

// ============================================================
if (piece == "coque") coque();
if (piece == "socle") socle();
